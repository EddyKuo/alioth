#include "ui/main_window.h"

#include <QFileDialog>
#include <QTimer>

#include "ui/annotation_properties_panel.h"
#include "ui/attachments_panel.h"
#include "ui/fields_panel.h"
#include "ui/layers_panel.h"
#include "ui/destinations_panel.h"
#include "ui/links_panel.h"
#include "ui/history_panel.h"
#include "engine/attachments/attachment_reader.h"
#include "engine/objects/pdf_source_document.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDockWidget>
#include <QInputDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QIcon>
#include <QListWidget>
#include <QPixmap>
#include <QTreeWidget>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QDesktopServices>
#include <QUrl>
#include <QMenuBar>
#include <QColorDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QSplitter>
#include <QTabBar>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#include "app/annotation_service.h"
#include "app/command_stack.h"
#include "app/page_operations_service.h"
#include "app/session.h"
#include "app/signature_controller.h"
#include "app/settings.h"

#include <memory>
#include "app/document_controller.h"
#include "app/document_email.h"
#include "app/selection_controller.h"
#include "platform/email_compose.h"
#include "platform/external_process.h"
#include "ui/external_tool_dialogs.h"
#include "engine/iosec/page_image_export.h"
#include "engine/iosec/text_export.h"
#include "ui/pan_zoom_panel.h"
#include "ui/loupe_widget.h"
#include "domain/table_extraction.h"
#include "engine/enhance/page_rasterizer.h"
#include <QClipboard>
#include "ui/ruler_widget.h"
#include <QGridLayout>
#include "ui/page_labels_dialog.h"
#include "engine/labels/page_label_codec.h"
#include "engine/objects/incremental_appender.h"
#include "platform/atomic_file.h"
#include <cstring>
#include "app/uisystem/tab_title_model.h"
#include "engine/pages/page_editor.h"
#include "domain/annotation_filter.h"
#include <QComboBox>
#include "ui/page_view.h"
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>

#include "app/print/print_service.h"
#include "platform/paths.h"
#include "ui/document_properties_dialog.h"
#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_bar.h"
#include "ui/ribbon/ribbon_default_layout.h"
#include "ui/ribbon/ribbon_model.h"
#include "ui/preferences_dialog.h"
#include "ui/ribbon_customize_dialog.h"
#include "app/annotation_clipboard.h"
#include "app/annotation_family_builders.h"
#include "app/attachment_service.h"
#include "app/measurement_service.h"
#include "engine/compare/document_comparer.h"
#include "app/form_build_service.h"
#include "app/annotation_flatten_service.h"
#include "app/redaction_service.h"
#include "app/comment_summary_service.h"
#include "app/fdf_io.h"
#include "domain/annotation_layout_tools.h"
#include "domain/page_range.h"
#include "domain/ink_pressure.h"
#include "app/xfdf_io.h"
#include "ui/compare_dialog.h"
#include "ui/sign_dialog.h"
#include "ui/sticky_note_popup.h"
#include "ui/signature_panel.h"
#include "ui/tags_panel.h"
#include "ui/order_panel.h"
#include "ui/inspector_panel.h"
#include "engine/objects/struct_tree_reader.h"
#include "engine/objects/accessibility_checker.h"
#include "app/accessibility_service.h"
#include "app/read_aloud_controller.h"

namespace alioth::ui {
namespace {

// QMainWindow::saveState / restoreState 的版本號。
// 面板預設版面有破壞性變更時 +1，舊的 windowState 會被 Qt 拒絕還原。
constexpr int kWindowStateVersion = 2;

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      controller_(new app::DocumentController(this)),
      selection_(new app::SelectionController(this)),
      annotations_(new app::AnnotationService(this)),
      accessibilityService_(new app::AccessibilityService(this)),
      readAloud_(new app::ReadAloudController(this)),
      commands_(new app::CommandStack(this)),
      settings_(new app::Settings(this)),
      pageOps_(new app::PageOperationsService(this)),
      signatures_(new app::SignatureController(this)),
      redaction_(new app::RedactionService(this)),
      annotationFlatten_(new app::AnnotationFlattenService(this)),
      stamps_(new app::StampService(this)),
      formBuild_(new app::FormBuildService(this)),
      attachmentService_(new app::AttachmentService(this)),
      actionRegistry_(new ribbon::ActionRegistry(this)),
      forms_(new app::FormController(this)),
      theme_(new app::ThemeController(this)) {
    setAcceptDrops(true);
    pageView_ = new PageView(controller_, selection_, this);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(pageView_);

    // Ribbon 放在中央區域的最上方，不用 setMenuWidget()。
    //
    // setMenuWidget() 會讓 QMainWindow **刪掉**原本的 QMenuBar，而選單列一死，
    // 它底下所有 QMenu 擁有的 QAction 也跟著死——包括工具列上引用同一批動作的
    // 按鈕，那些按鈕會變成沒有動作的空殼（鍵盤與滑鼠都按不出任何反應），
    // 而且切回傳統選單時「檔案／檢視／編輯／組織／註解」全部不見。
    // 這個症狀完全沒有錯誤訊息，是無障礙稽核掃出「五個工具列按鈕鍵盤不可達」
    // 才追到的。
    // 尺規（PRD-VIEW-015）。放在 PageView 的上緣與左緣，用網格版面對齊：
    // 上尺規要與檢視區「同一個 x 原點」，左尺規要與檢視區「同一個 y 原點」，
    // 而左上角那一格必須是空的，否則兩把尺規的零點會各差一個尺規厚度。
    horizontalRuler_ = new RulerWidget(Qt::Horizontal, this);
    verticalRuler_ = new RulerWidget(Qt::Vertical, this);
    auto* rulerCorner = new QWidget(this);
    rulerCorner->setFixedSize(verticalRuler_->sizeHint().width(),
                              horizontalRuler_->sizeHint().height());

    // 文件頁籤（PRD-UI-001）。跨整個網格的最上一列：它是這個視窗開了哪些
    // 文件的清單，不屬於某一把尺規或某一個檢視。
    documentTabs_ = new QTabBar(this);
    documentTabs_->setObjectName(QStringLiteral("documentTabs"));
    documentTabs_->setAccessibleName(tr("開啟的文件"));
    // Qt 的 QAccessibleTabBar 把 Name 回報成「目前那一頁的標題」，所以沒有
    // 文件時它是空的——螢幕閱讀器對著這條列什麼都念不出來。setAccessibleName()
    // 不會蓋掉它（名字來自 interface，不是 widget 屬性），必須自己接一個工廠。
    //
    // 工廠只認 objectName 為 documentTabs 的那一個，其餘 QTabBar（例如
    // QMainWindow 自己為 tabify 的停靠面板產生的那些）一律回 nullptr 讓
    // Qt 的預設實作接手——換掉它們會影響停靠面板的既有行為。
    static const bool tabBarAccessibleFactoryInstalled = [] {
        QAccessible::installFactory([](const QString& className,
                                       QObject* object) -> QAccessibleInterface* {
            if (className != QLatin1String("QTabBar") || object == nullptr ||
                !object->isWidgetType() ||
                object->objectName() != QLatin1String("documentTabs")) {
                return nullptr;
            }
            class DocumentTabsAccessible : public QAccessibleWidget {
            public:
                explicit DocumentTabsAccessible(QWidget* widget)
                    : QAccessibleWidget(widget, QAccessible::PageTabList) {}
                QString text(QAccessible::Text type) const override {
                    if (type == QAccessible::Name && !widget()->accessibleName().isEmpty()) {
                        return widget()->accessibleName();
                    }
                    return QAccessibleWidget::text(type);
                }
            };
            return new DocumentTabsAccessible(static_cast<QWidget*>(object));
        });
        return true;
    }();
    (void)tabBarAccessibleFactoryInstalled;
    documentTabs_->setTabsClosable(true);
    documentTabs_->setMovable(true);
    documentTabs_->setExpanding(false);
    documentTabs_->setDocumentMode(true);
    documentTabs_->setUsesScrollButtons(true);
    // 沒有文件時不佔一條空白列。
    documentTabs_->hide();

    auto* viewArea = new QWidget(this);
    auto* viewGrid = new QGridLayout(viewArea);
    viewGrid->setContentsMargins(0, 0, 0, 0);
    viewGrid->setSpacing(0);
    viewGrid->addWidget(documentTabs_, 0, 0, 1, 2);
    viewGrid->addWidget(rulerCorner, 1, 0);
    viewGrid->addWidget(horizontalRuler_, 1, 1);
    viewGrid->addWidget(verticalRuler_, 2, 0);
    viewGrid->addWidget(splitter_, 2, 1);
    viewGrid->setRowStretch(2, 1);
    viewGrid->setColumnStretch(1, 1);

    connect(documentTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (syncingTabs_ || index < 0) return;
        const app::WindowState* window = tabs_.window(tabs_.primaryWindowId());
        if (window == nullptr || index >= static_cast<int>(window->tabs.size())) return;
        tabs_.setActiveTab(tabs_.primaryWindowId(), index);
        loadDocument(window->tabs[static_cast<std::size_t>(index)].documentId);
    });
    connect(documentTabs_, &QTabBar::tabCloseRequested, this,
            [this](int index) { closeTab(index); });
    // 分離成獨立視窗（PRD-UI-001）。走右鍵選單而不是拖曳：拖曳出視窗邊界
    // 的判定在多螢幕與高 DPI 下不穩定，而使用者一旦不小心拖出去，找不回來
    // 的成本遠高於多按一次右鍵。合併回來就是在新視窗裡關掉頁籤——它是
    // 主視窗以外的視窗，關到剩零個頁籤時視窗本身會關閉。
    documentTabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(documentTabs_, &QTabBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = documentTabs_->tabAt(pos);
        if (index < 0) return;
        QMenu menu(this);
        QAction* detach = menu.addAction(tr("在新視窗開啟"));
        detach->setEnabled(documentTabs_->count() > 1);
        QAction* close = menu.addAction(tr("關閉頁籤"));
        QAction* chosen = menu.exec(documentTabs_->mapToGlobal(pos));
        if (chosen == detach) {
            detachTab(index);
        } else if (chosen == close) {
            closeTab(index);
        }
    });

    connect(documentTabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (syncingTabs_) return;
        // 只更新模型，不重畫頁籤：QTabBar 已經自己搬好了，再 sync 一次會
        // 在拖曳結束的當下閃一下。
        (void)tabs_.reorderTab(tabs_.primaryWindowId(), from, to);
    });

    // 預設不顯示。尺規對多數閱讀情境是干擾，需要的人自己開。
    horizontalRuler_->hide();
    verticalRuler_->hide();
    rulerCorner->hide();
    rulerCorner_ = rulerCorner;

    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(viewArea);
    setCentralWidget(central);

    // 尺規的刻度要跟著捲動與縮放走，否則刻度會與頁面錯開——
    // 一把對不準的尺規比沒有尺規更糟，因為使用者會照著它量。
    const auto syncRulers = [this] {
        if (!horizontalRuler_->isVisible()) return;
        const domain::PageTransform transform = pageView_->currentPageTransform();
        const QPoint origin = pageView_->currentPageOriginInViewport();
        horizontalRuler_->setMapping(transform, origin.x());
        verticalRuler_->setMapping(transform, origin.y());
    };
    connect(pageView_, &PageView::viewportChanged, this, syncRulers);
    connect(pageView_, &PageView::scaleChanged, this, [syncRulers](double) { syncRulers(); });
    connect(pageView_, &PageView::pageChanged, this, [syncRulers](int) { syncRulers(); });

    // 從尺規拖出參考線。放開才真的加進去——拖曳過程中的每一格都加一條的話，
    // 使用者拉一次會得到幾十條參考線。
    connect(horizontalRuler_, &RulerWidget::guideCommitted, this, [this](double positionPt) {
        pageView_->guides().add(
            domain::GuideLine{domain::GuideOrientation::Horizontal, positionPt, false});
        pageView_->setGuidesVisible(true);
    });
    connect(verticalRuler_, &RulerWidget::guideCommitted, this, [this](double positionPt) {
        pageView_->guides().add(
            domain::GuideLine{domain::GuideOrientation::Vertical, positionPt, false});
        pageView_->setGuidesVisible(true);
    });

    buildActions();
    buildDockPanels();
    buildStatusBar();
    buildPanelActions();
    buildRibbon();
    rebuildExternalToolsToolbar();

    setWindowTitle(tr("Alioth PDF 審閱工作站"));
    resize(1280, 860);

    history_.load();
    historyPanel_->reload();

    applySettings();
    restoreSession();

    connect(controller_, &app::DocumentController::documentOpened, this,
            [this](const QString& path) {
                updateWindowTitle(path);
                // 之後所有寫入都以這一刻的檔案狀態為基準（PRD-IO-004）。
                fileSnapshot_ = platform::captureSnapshot(path);
                applyPermissionRestrictions();
                const auto& info = controller_->info();
                pageLabel_->setText(tr("第 1 / %1 頁").arg(info.pageCount));

                // 不支援的文件類型必須明確提示，且不得誤導（PRD §13）。
                QStringList notices;
                if (info.hasXfa) notices << tr("XFA 表單：僅顯示後備內容");
                if (info.hasJavaScript) notices << tr("內嵌 JavaScript：不執行");
                if (info.hasSignatures) {
                    notices << tr("含數位簽章");
                    // 驗證要算整份檔案的雜湊，只在真的有簽章時才做。
                    signaturePanel_->refresh();
                }
                noticeLabel_->setText(notices.join(QStringLiteral("　|　")));
                populateThumbnails();
                recordHistory(path);
                reloadNavigationPanels(path);
                controller_->requestLayers();
                forms_->openDocument(path);
                if (pendingRestorePage_ >= 0) {
                    // 續讀位置。頁數在這裡才知道，越界時停在最後一頁而不是拒絕——
                    // 文件被編輯過而變短是正常的。
                    pageView_->setPageIndex(std::min(pendingRestorePage_, info.pageCount - 1));
                    if (pendingRestoreScale_ > 0.0) pageView_->setScale(pendingRestoreScale_);
                    pendingRestorePage_ = -1;
                    pendingRestoreScale_ = 0.0;
                }
                // 先掃前幾頁的註解就好；捲動到哪再補（PRD-ANN-008 的 500 毫秒預算）。
                controller_->requestAnnotations(0, std::min(info.pageCount - 1, 32));
            });

    connect(selection_, &app::SelectionController::searchHitsChanged, this, [this] {
        const auto& hits = selection_->searchHits();
        // 只補上新到的那幾筆，不整份重建——重建會讓使用者正在看的捲動位置跳掉。
        for (std::size_t i = static_cast<std::size_t>(searchResults_->count()); i < hits.size();
             ++i) {
            const auto& hit = hits[i];
            new QListWidgetItem(tr("第 %1 頁：%2").arg(hit.pageIndex + 1).arg(hit.context),
                                searchResults_);
        }
    });

    connect(selection_, &app::SelectionController::searchFinished, this,
            [this](int total, bool cancelled) {
                statusBar()->showMessage(cancelled ? tr("搜尋已取消")
                                                   : tr("找到 %1 筆").arg(total), 5000);
            });

    // 建索引的進度（PRD-SRCH-001 / ADR-005）。
    //
    // 顯示它不是為了好看：索引沒建完時搜尋會退回逐頁掃描，那條路徑在 500 頁工程圖上
    // 要好幾秒。使用者需要知道「現在慢是因為還在準備」，而不是以為程式卡住了。
    connect(selection_, &app::SelectionController::searchIndexProgress, this,
            [this](int indexed, int total) {
                if (total <= 0) return;
                if (indexed >= total) {
                    statusBar()->showMessage(tr("搜尋索引已就緒"), 3000);
                } else {
                    statusBar()->showMessage(
                        tr("建立搜尋索引中… %1 / %2 頁").arg(indexed).arg(total));
                }
            });

    connect(selection_, &app::SelectionController::selectionChanged, this, [this] {
        const auto& current = selection_->selection();
        if (current.isEmpty()) {
            statusBar()->clearMessage();
        } else {
            statusBar()->showMessage(tr("已選取 %1 個字元").arg(current.range.end -
                                                              current.range.start));
        }
    });

    connect(controller_, &app::DocumentController::outlineReady, this,
            [this] { populateOutline(); });

    connect(controller_, &app::DocumentController::annotationsReady, this,
            [this] { populateAnnotations(); });

    connect(controller_, &app::DocumentController::thumbnailReady, this,
            [this](int pageIndex, const QImage& image) {
                if (pageIndex < 0 || pageIndex >= thumbnailList_->count()) return;
                thumbnailList_->item(pageIndex)->setIcon(QIcon(QPixmap::fromImage(image)));
            });

    connect(controller_, &app::DocumentController::autosaved, this, [this](const QString&) {
        statusBar()->showMessage(tr("已自動儲存"), 3000);
    });

    connect(controller_, &app::DocumentController::autosaveFailed, this,
            [this](const QString& message) {
                // 自動儲存失敗不能只寫進 log：使用者以為有備份，實際沒有。
                statusBar()->showMessage(tr("自動儲存失敗：%1").arg(message), 10000);
            });

    connect(controller_, &app::DocumentController::documentOpenFailed, this,
            [this](int, const QString& message) {
                QMessageBox::warning(this, tr("無法開啟文件"), message);
            });

    connect(pageView_, &PageView::scaleChanged, this, [this](double scale) {
        zoomLabel_->setText(tr("%1%").arg(static_cast<int>(scale * 100.0)));
    });

    connect(pageView_, &PageView::linkActivated, this,
            [this](const domain::LinkTarget& target) { followLink(target); });

    // PRD-ANN-004 的另一個方向：點頁面上的註解 → 開註釋視窗並選中清單那一列。
    // 命中測試在這裡而不是 PageView 裡，因為 PageView 刻意不認識註解。
    connect(pageView_, &PageView::pageClicked, this,
            [this](int pageIndex, const domain::PointF& pagePoint) {
                if (pageView_->tool() == Tool::SelectComment) {
                    // 點在空白處就取消選取。保留上一個選取會讓「刪除」作用在
                    // 一則使用者已經不認為自己選著的註解上。
                    selectAnnotation(annotationAtForSelection(pageIndex, pagePoint));
                    return;
                }
                const std::size_t index = annotationAt(pageIndex, pagePoint);
                if (index < controller_->annotations().size()) openNotePopup(index);
            });

    // Delete 作用在頁面上（「選取註解」工具選中之後）。掛在 pageView_ 而不是
    // 視窗層級：焦點在頁面上按 Delete 的意圖是刪掉選中的註解，而清單也有
    // 一個同名動作——掛在視窗層級兩者會互搶。
    auto* deleteOnPageAction = new QAction(tr("刪除選取的註解"), pageView_);
    deleteOnPageAction->setShortcut(QKeySequence::Delete);
    deleteOnPageAction->setShortcutContext(Qt::WidgetShortcut);
    pageView_->addAction(deleteOnPageAction);
    connect(deleteOnPageAction, &QAction::triggered, this, [this] {
        if (pageView_->tool() != Tool::SelectComment) return;
        if (selectedAnnotation_ >= controller_->annotations().size()) return;
        deleteSelectedAnnotation();
    });

    connect(pageView_, &PageView::pageDoubleClicked, this,
            [this](int pageIndex, const domain::PointF& pagePoint) {
                const std::size_t index = annotationAtForSelection(pageIndex, pagePoint);
                if (index >= controller_->annotations().size()) return;
                selectAnnotation(index);
                openNotePopup(index);
            });

    // PRD-TXT-003 / PRD-TXT-005：框選之後做什麼，取決於目前是哪個工具。
    connect(pageView_, &PageView::areaSelected, this,
            [this](int pageIndex, const domain::RectF& pageRect) {
                if (pageView_->tool() == Tool::Snapshot) {
                    copySnapshotToClipboard(pageIndex, pageRect);
                } else if (pageView_->tool() == Tool::RedactMark) {
                    markRedaction(pageIndex, pageRect);
                } else if (pageView_->tool() == Tool::FormField) {
                    createFormField(pageIndex, pageRect);
                } else if (pageView_->tool() == Tool::Attachment) {
                    attachFileAt(pageIndex, pageRect);
                } else {
                    copyAreaTextAsTsv(pageIndex, pageRect);
                }
            });

    // 多邊形與折線（PRD-ANN-002）。與 shapeDrawn 分開：頂點數由使用者決定，
    // 沒辦法用一個矩形表達。
    connect(pageView_, &PageView::verticesDrawn, this,
            [this](int pageIndex, const std::vector<domain::PointF>& vertices, bool closed) {
                domain::Annotation annotation;
                annotation.color = domain::ColorRgb{0.85, 0.1, 0.1};
                if (closed) {
                    domain::PolygonGeometry polygon;
                    polygon.vertices = vertices;
                    if (pageView_->tool() == Tool::Cloud) {
                        polygon.borderEffect.effect = domain::BorderEffect::Cloudy;
                        polygon.borderEffect.intensity = 1.0;
                    }
                    annotation.geometry = std::move(polygon);
                } else {
                    domain::PolyLineGeometry poly;
                    poly.vertices = vertices;
                    annotation.geometry = poly;
                }
                // /Rect 是所有頂點的外接矩形。產生器不會自己算，缺了它
                // 註解在多數檢視器裡點不到（命中測試用的是 /Rect）。
                domain::RectF bounds{vertices.front().x, vertices.front().y, vertices.front().x,
                                     vertices.front().y};
                for (const domain::PointF& vertex : vertices) {
                    bounds.left = std::min(bounds.left, vertex.x);
                    bounds.bottom = std::min(bounds.bottom, vertex.y);
                    bounds.right = std::max(bounds.right, vertex.x);
                    bounds.top = std::max(bounds.top, vertex.y);
                }
                annotation.rect = bounds;

                app::AnnotationRequest request;
                request.pageIndex = pageIndex;
                request.annotation = annotation;
                const QString label = !closed ? tr("折線")
                                     : pageView_->tool() == Tool::Cloud ? tr("雲線")
                                                                        : tr("多邊形");
                commitAnnotation(std::move(request), label);
            });

    connect(pageView_, &PageView::shapeDrawn, this,
            [this](int pageIndex, const domain::RectF& rect) { applyShape(pageIndex, rect); });

    connect(pageView_, &PageView::markupSelectionCompleted, this, [this] {
        switch (pageView_->tool()) {
            case Tool::Highlight:
                applyTextMarkup(domain::TextMarkupKind::Highlight, tr("螢光筆"));
                break;
            case Tool::Underline:
                applyTextMarkup(domain::TextMarkupKind::Underline, tr("底線"));
                break;
            case Tool::StrikeOut:
                applyTextMarkup(domain::TextMarkupKind::StrikeOut, tr("刪除線"));
                break;
            case Tool::Squiggly:
                applyTextMarkup(domain::TextMarkupKind::Squiggly, tr("波浪線"));
                break;
            default:
                break;
        }
    });

    connect(pageView_, &PageView::strokeDrawn, this,
            [this](int pageIndex, const std::vector<domain::PressurePoint>& points) {
                applyPencilStroke(pageIndex, points);
            });

    connect(pageView_, &PageView::pageChanged, this, [this](int index) {
        pageLabel_->setText(tr("第 %1 / %2 頁").arg(index + 1).arg(controller_->pageCount()));
        // 翻頁之後尺寸可能不同（混合尺寸的文件很常見），但游標未必在頁面上，
        // 所以只更新尺寸那一半。
        updateGeometryLabel(index, nullptr);
        // Order 面板的比對是逐頁的（PRD-A11Y-002），換頁要重算。
        orderPanel_->setPageIndex(index);
        // 連結是逐頁向引擎要的。面板收起來時不要求——翻頁本來就在跟渲染
        // 佇列搶時間，為了一個沒人在看的面板多排一件事並不划算。
        if (linksPanel_ != nullptr && linksDock_ != nullptr && linksDock_->isVisible()) {
            linksPanel_->setPage(index);
        }
    });

    // PRD-UI-013：觸控長按等同右鍵開選單。選單內容刻意精簡——目前只提供
    // 最高頻的審閱動作（螢光筆），與 docs/UX_CONCEPT.md 的「審閱動作優先於
    // 一切」呼應；之後有了完整的滑鼠右鍵選單再讓兩者共用同一份建構邏輯。
    connect(pageView_, &PageView::longPressTriggered, this,
            [this](int, const domain::PointF&, const QPoint& widgetPoint) {
                QMenu menu(this);
                QAction* highlight = menu.addAction(tr("螢光筆"));
                highlight->setEnabled(!selection_->selection().isEmpty());
                connect(highlight, &QAction::triggered, this, [this] { applyHighlight(); });
                menu.exec(pageView_->viewport()->mapToGlobal(widgetPoint));
            });
}

MainWindow::~MainWindow() = default;

void MainWindow::buildActions() {
    // 選單列的鍵盤入口是 Alt 與 F10，不是 Tab；但它仍然需要身分與名稱，
    // 否則 UIA 掃出來的樹頂端是一個無名節點。
    menuBar()->setObjectName(QStringLiteral("mainMenuBar"));
    menuBar()->setAccessibleName(tr("主選單"));

    auto* fileMenu = menuBar()->addMenu(tr("檔案(&F)"));
    auto* viewMenu = menuBar()->addMenu(tr("檢視(&V)"));

    auto* openAction = fileMenu->addAction(tr("開啟(&O)..."));
    registerRibbonAction(QStringLiteral("file.open"), openAction);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("開啟 PDF"), QString(),
                                                          tr("PDF 文件 (*.pdf)"));
        if (!path.isEmpty()) openPath(path);
    });

    auto* propertiesAction = fileMenu->addAction(tr("文件屬性(&I)..."));
    registerRibbonAction(QStringLiteral("file.properties"), propertiesAction);
    propertiesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(propertiesAction, &QAction::triggered, this, [this] {
        if (!controller_->isOpen()) {
            QMessageBox::information(this, tr("文件屬性"), tr("尚未開啟文件"));
            return;
        }
        DocumentPropertiesDialog dialog(currentPath_, controller_->info(), this);
        dialog.exec();
    });

    recentMenu_ = fileMenu->addMenu(tr("最近開啟(&R)"));
    // Ribbon 的預設配置用 file.recent 指這個子選單。QMenu 的 menuAction()
    // 就是那顆按鈕，掛上去按下會展開清單——不需要另做一份。
    registerRibbonAction(QStringLiteral("file.recent"), recentMenu_->menuAction());
    connect(settings_, &app::Settings::recentFilesChanged, this,
            [this] { rebuildRecentMenu(); });
    rebuildRecentMenu();

    auto* printAction = fileMenu->addAction(tr("列印(&P)..."));
    printAction->setShortcut(QKeySequence::Print);
    connect(printAction, &QAction::triggered, this, [this] { printDocument(); });
    registerRibbonAction(QStringLiteral("file.print"), printAction);

    // PRD-IO-007：匯出頁面為影像、匯出純文字。引擎側早就完成並測過，
    // 只是一直沒有入口——沒有入口的功能對使用者而言不存在。
    // PRD-UI-017：文件重新命名。放在檔案選單，與「另存新檔」相鄰——
    // 使用者想改名時第一個看的就是這兩個之間。
    // 列印預覽（PRD-IO-008）。與列印共用同一個 PrintService，只是把輸出
    // 導到預覽視窗——兩條各自渲染的路徑會讓「預覽看到的」與「印出來的」
    // 慢慢分岔，而那種分岔要印出來才會發現。
    auto* printPreviewAction = fileMenu->addAction(tr("列印預覽(&W)..."));
    registerRibbonAction(QStringLiteral("file.printPreview"), printPreviewAction);
    connect(printPreviewAction, &QAction::triggered, this, [this] { showPrintPreview(); });

    auto* renameAction = fileMenu->addAction(tr("重新命名(&N)..."));
    registerRibbonAction(QStringLiteral("file.rename"), renameAction);
    connect(renameAction, &QAction::triggered, this, [this] { renameCurrentDocument(); });

    auto* exportImageAction = fileMenu->addAction(tr("匯出頁面為影像(&G)..."));
    registerRibbonAction(QStringLiteral("file.exportImage"), exportImageAction);
    connect(exportImageAction, &QAction::triggered, this, [this] { exportCurrentPageImage(); });

    auto* exportTextAction = fileMenu->addAction(tr("匯出純文字(&T)..."));
    registerRibbonAction(QStringLiteral("file.exportText"), exportTextAction);
    connect(exportTextAction, &QAction::triggered, this, [this] { exportDocumentText(); });

    // PRD-IO-011：Email 文件。附件是整份 PDF，不是表單匯出資料——
    // 沿用 WP34 的 Simple MAPI 封裝（platform::email_compose），不另寫寄送機制。
    auto* emailAction = fileMenu->addAction(tr("以電子郵件傳送(&M)..."));
    registerRibbonAction(QStringLiteral("file.email"), emailAction);
    connect(emailAction, &QAction::triggered, this, [this] { emailCurrentDocument(); });

    // PRD-UI-016：第三方程式工具列。管理入口放在檔案選單，不論工具列目前
    // 是否顯示（清單為空時工具列本身不出現），使用者都找得到設定畫面。
    auto* manageToolsAction = fileMenu->addAction(tr("管理外部程式(&X)..."));
    registerRibbonAction(QStringLiteral("file.manageExternalTools"), manageToolsAction);
    connect(manageToolsAction, &QAction::triggered, this, [this] {
        auto* dialog = new ExternalToolManagerDialog(settings_, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &ExternalToolManagerDialog::toolsChanged, this,
                [this] { rebuildExternalToolsToolbar(); });
        dialog->exec();
    });

    auto* closeAction = fileMenu->addAction(tr("關閉(&C)"));
    registerRibbonAction(QStringLiteral("file.close"), closeAction);
    connect(closeAction, &QAction::triggered, this, [this] {
        controller_->closeDocument();
        updateWindowTitle({});
    });

    auto* editMenu = menuBar()->addMenu(tr("編輯(&E)"));
    undoAction_ = editMenu->addAction(tr("復原"));
    registerRibbonAction(QStringLiteral("edit.undo"), undoAction_);
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setEnabled(false);
    connect(undoAction_, &QAction::triggered, this, [this] {
        commands_->undo();
        updateUndoActions();
    });

    redoAction_ = editMenu->addAction(tr("重做"));
    registerRibbonAction(QStringLiteral("edit.redo"), redoAction_);
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setEnabled(false);
    connect(redoAction_, &QAction::triggered, this, [this] {
        commands_->redo();
        updateUndoActions();
    });

    editMenu->addSeparator();
    auto* selectAllAction = editMenu->addAction(tr("全選這一頁的文字(&A)"));
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    registerRibbonAction(QStringLiteral("edit.selectAll"), selectAllAction);
    connect(selectAllAction, &QAction::triggered, this, [this] {
        if (!controller_->isOpen()) return;
        selection_->selectAllOnPage(pageView_->pageIndex());
    });

    // 剪下與貼上作用在**註解**上，不是頁面內文——內文編輯是 PRD §2.1 明確
    // 排除的範圍（PDFium 沒有文字重排能力）。剪下＝複製後刪除，兩步都走
    // 既有的路徑，不另寫一條。
    auto* cutAction = editMenu->addAction(tr("剪下註解(&T)"));
    cutAction->setShortcut(QKeySequence::Cut);
    registerRibbonAction(QStringLiteral("edit.cut"), cutAction);
    connect(cutAction, &QAction::triggered, this, [this] {
        if (annotationList_->currentRow() < 0) {
            statusBar()->showMessage(tr("請先在註解清單選一則註解"), 4000);
            return;
        }
        copySelectedAnnotation();
        deleteSelectedAnnotation();
    });

    auto* pasteAction = editMenu->addAction(tr("貼上註解(&P)"));
    pasteAction->setShortcut(QKeySequence::Paste);
    registerRibbonAction(QStringLiteral("edit.paste"), pasteAction);
    connect(pasteAction, &QAction::triggered, this, [this] { pasteAnnotations(); });

    editMenu->addSeparator();
    // 工具持續模式（PRD-UI-007）。預設關閉：用完一個註解工具就切回選取，
    // 避免使用者在下一次點擊時誤加一個註解。
    auto* stickyToolAction = editMenu->addAction(tr("工具持續模式(&K)"));
    stickyToolAction->setCheckable(true);
    stickyToolAction->setChecked(settings_->stickyTools());
    registerRibbonAction(QStringLiteral("tool.persistent"), stickyToolAction);
    connect(stickyToolAction, &QAction::toggled, this, [this](bool on) {
        settings_->setStickyTools(on);
        toolPersistence_.setStickyEnabled(on);
    });
    toolPersistence_.setStickyEnabled(settings_->stickyTools());

    auto* organizeMenu = menuBar()->addMenu(tr("組織(&G)"));
    buildOrganizeMenu(organizeMenu);

    auto* commentMenu = menuBar()->addMenu(tr("註解(&C)"));
    auto* highlightAction = commentMenu->addAction(tr("螢光筆(&H)"));
    registerRibbonAction(QStringLiteral("annot.highlight"), highlightAction);
    highlightAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(highlightAction, &QAction::triggered, this, [this] { applyHighlight(); });

    // 註解交換（PRD-ANN-013）。XFDF 與 FDF 都做，由副檔名決定格式：
    // Acrobat 的「匯出註解」預設給的是 FDF，只認 XFDF 等於收不到多數來稿。
    commentMenu->addSeparator();
    auto* importAnnotationsAction = commentMenu->addAction(tr("匯入註解(&I)..."));
    registerRibbonAction(QStringLiteral("comment.import"), importAnnotationsAction);
    connect(importAnnotationsAction, &QAction::triggered, this,
            [this] { importAnnotationsFromFile(); });
    auto* exportAnnotationsAction = commentMenu->addAction(tr("匯出註解(&E)..."));
    registerRibbonAction(QStringLiteral("comment.export"), exportAnnotationsAction);
    connect(exportAnnotationsAction, &QAction::triggered, this,
            [this] { exportAnnotationsToFile(ExportScope::All); });
    // 匯出選定註解（PRD-ANN-013）。獨立一個項目而不是讓「匯出註解」依選取
    // 狀態自己變聰明：那樣同一個選單項目會依畫面上某處的狀態產出兩種不同的
    // 檔案，而使用者不見得記得自己在清單裡點過什麼。
    auto* exportSelectedAction = commentMenu->addAction(tr("匯出選定註解..."));
    registerRibbonAction(QStringLiteral("comment.exportSelected"), exportSelectedAction);
    connect(exportSelectedAction, &QAction::triggered, this,
            [this] { exportAnnotationsToFile(ExportScope::SelectedOnly); });

    // 註解摘要（PRD-ANN-028）。三種版面各一個入口而不是一個對話框讓使用者選：
    // 三者的輸出物完全不同（一份文字檔 vs 一份新 PDF），混在同一個流程裡
    // 會讓「按下確定之後會拿到什麼」變得不可預測。
    auto* summaryMenu = commentMenu->addMenu(tr("摘要註解(&S)"));
    auto* summaryOnlyAction = summaryMenu->addAction(tr("僅摘要（純文字）..."));
    registerRibbonAction(QStringLiteral("comment.summaryOnly"), summaryOnlyAction);
    // Ribbon 預設配置用的是 comment.summarize 這個較短的 id。同一個 QAction 註冊
    // 在兩個 id 底下是可以的，比在配置與程式之間硬選一個名字好——改名會讓
    // 使用者已經存下的 ribbon.json 少一顆按鈕。
    registerRibbonAction(QStringLiteral("comment.summarize"), summaryOnlyAction);
    connect(summaryOnlyAction, &QAction::triggered, this, [this] { exportCommentSummary(); });
    auto* summaryWithDocAction = summaryMenu->addAction(tr("文件加摘要（每頁後插入）..."));
    registerRibbonAction(QStringLiteral("comment.summaryWithDocument"), summaryWithDocAction);
    connect(summaryWithDocAction, &QAction::triggered, this,
            [this] { exportDocumentWithSummary(SummaryLayout::InsertAfterEachPage); });
    auto* summarySideBySideAction = summaryMenu->addAction(tr("並排（左原文、右摘要）..."));
    registerRibbonAction(QStringLiteral("comment.summarySideBySide"), summarySideBySideAction);
    connect(summarySideBySideAction, &QAction::triggered, this,
            [this] { exportDocumentWithSummary(SummaryLayout::SideBySide); });

    // 上一則／下一則註解（PDF-XChange 的 Previous / Next Comment）。
    // 審閱一份兩百則註解的文件時，這是最常按的兩個鍵——沒有它，使用者
    // 只能在清單面板上逐列點，而清單的排序未必是文件順序。
    commentMenu->addSeparator();
    auto* previousCommentAction = commentMenu->addAction(tr("上一則註解"));
    previousCommentAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    registerRibbonAction(QStringLiteral("comment.previous"), previousCommentAction);
    connect(previousCommentAction, &QAction::triggered, this,
            [this] { goToAdjacentAnnotation(-1); });

    auto* nextCommentAction = commentMenu->addAction(tr("下一則註解"));
    nextCommentAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Period));
    registerRibbonAction(QStringLiteral("comment.next"), nextCommentAction);
    connect(nextCommentAction, &QAction::triggered, this, [this] { goToAdjacentAnnotation(1); });

    // 顯示／隱藏所有註解（PDF-XChange 的 Show Comments）。
    //
    // 這是純檢視選項，不改文件——關掉之後看到的是「原稿長什麼樣」，
    // 那是審閱到一半最常需要的一個對照。實作是 FPDF_ANNOT 旗標，
    // 因此連表單欄位的外觀也會一起隱藏，這與 Acrobat 的行為一致。
    auto* showAnnotationsAction = commentMenu->addAction(tr("顯示所有註解"));
    showAnnotationsAction->setCheckable(true);
    showAnnotationsAction->setChecked(true);
    registerRibbonAction(QStringLiteral("comment.showAll"), showAnnotationsAction);
    connect(showAnnotationsAction, &QAction::toggled, this, [this](bool on) {
        controller_->setAnnotationsVisible(on);
        statusBar()->showMessage(on ? tr("已顯示所有註解") : tr("已隱藏所有註解（不影響檔案）"),
                                 4000);
    });

    // 攤平註解（PRD-ANN-013）。放在摘要下面而不是與匯入匯出並列：後兩者是
    // 可逆的資料搬運，攤平之後那些標記在任何檢視器裡都不再是註解。
    auto* flattenAction = commentMenu->addAction(tr("攤平註解(&F)..."));
    registerRibbonAction(QStringLiteral("comment.flatten"), flattenAction);
    connect(flattenAction, &QAction::triggered, this, [this] { flattenAnnotations(); });

    // 複製選取的文字（PRD-TXT-002）。edit.copy 先前是停用的佔位按鈕。
    auto* copyAction = commentMenu->addAction(tr("複製選取的文字"));
    copyAction->setShortcut(QKeySequence::Copy);
    registerRibbonAction(QStringLiteral("edit.copy"), copyAction);
    connect(copyAction, &QAction::triggered, this, [this] { copySelectionToClipboard(); });

    // 工具切換（PRD-UI-002 的「工具」群組、PRD-TXT-003、PRD-TXT-005）。
    //
    // 這一整組先前是停用的佔位按鈕——PageView::setTool 沒有任何呼叫者，
    // 所以矩形、橢圓、便利貼、快照全都按不出來。互斥群組讓「目前是哪個工具」
    // 在畫面上永遠只有一個答案；沒有互斥的話兩顆按鈕會同時看起來是按下的。
    auto* toolMenu = commentMenu->addMenu(tr("工具"));
    auto* toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);
    const auto addTool = [&](const QString& text, Tool tool, const QString& id,
                             const QKeySequence& shortcut, bool checked) {
        auto* action = toolMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool] { pageView_->setTool(tool); });
        registerRibbonAction(id, action);
        return action;
    };

    addTool(tr("選取文字"), Tool::Select, QStringLiteral("tool.select"),
            QKeySequence(Qt::Key_V), true);
    addTool(tr("手形"), Tool::Hand, QStringLiteral("tool.hand"), QKeySequence(Qt::Key_H), false);
    // 選取註解（PDF-XChange 的 Select Comments Tool）。與「選取文字」分成兩個
    // 工具而不是讓選取工具兼差：選取工具點在螢光筆上時的意圖是從那裡開始選字，
    // 兩個意圖互斥。快捷鍵 C 對齊 PDF-XChange。
    addTool(tr("選取註解"), Tool::SelectComment, QStringLiteral("tool.selectComments"),
            QKeySequence(Qt::Key_C), false);
    addTool(tr("快照"), Tool::Snapshot, QStringLiteral("tool.snapshot"), QKeySequence(), false);
    registerRibbonAction(
        QStringLiteral("protect.redactMark"),
        addTool(tr("標記待塗黑"), Tool::RedactMark, QStringLiteral("tool.redactMark"),
                QKeySequence(), false));
    addTool(tr("區域選取"), Tool::AreaSelect, QStringLiteral("tool.areaSelect"), QKeySequence(),
            false);
    toolMenu->addSeparator();
    addTool(tr("矩形"), Tool::Rectangle, QStringLiteral("annot.rectangle"), QKeySequence(), false);
    addTool(tr("橢圓"), Tool::Ellipse, QStringLiteral("annot.ellipse"), QKeySequence(), false);
    addTool(tr("便利貼"), Tool::StickyNote, QStringLiteral("annot.stickyNote"), QKeySequence(),
            false);
    addTool(tr("直線"), Tool::Line, QStringLiteral("annot.line"), QKeySequence(), false);
    addTool(tr("箭頭"), Tool::Arrow, QStringLiteral("annot.arrow"), QKeySequence(), false);
    addTool(tr("多邊形"), Tool::Polygon, QStringLiteral("annot.polygon"), QKeySequence(), false);
    addTool(tr("折線"), Tool::PolyLine, QStringLiteral("annot.polyline"), QKeySequence(), false);
    addTool(tr("雲線"), Tool::Cloud, QStringLiteral("annot.cloud"), QKeySequence(), false);
    // Ribbon 預設配置把鉛筆叫做 annot.ink（對應 PDF 的 /Ink 子型），選單這邊
    // 叫鉛筆。兩個 id 指同一顆動作，不為了名字不同再做一顆。
    registerRibbonAction(
        QStringLiteral("annot.ink"),
        addTool(tr("鉛筆"), Tool::Pencil, QStringLiteral("annot.pencil"), QKeySequence(), false));
    addTool(tr("底線"), Tool::Underline, QStringLiteral("annot.underline"), QKeySequence(), false);
    addTool(tr("刪除線"), Tool::StrikeOut, QStringLiteral("annot.strikeout"), QKeySequence(),
            false);
    addTool(tr("波浪線"), Tool::Squiggly, QStringLiteral("annot.squiggly"), QKeySequence(), false);
    addTool(tr("文字方塊"), Tool::TextBox, QStringLiteral("annot.textBox"), QKeySequence(), false);
    addTool(tr("打字機"), Tool::Typewriter, QStringLiteral("annot.typewriter"), QKeySequence(),
            false);
    addTool(tr("指示框"), Tool::Callout, QStringLiteral("annot.callout"), QKeySequence(), false);
    addTool(tr("圖章"), Tool::Stamp, QStringLiteral("annot.stamp"), QKeySequence(), false);
    addTool(tr("校正符號"), Tool::Caret, QStringLiteral("annot.caret"), QKeySequence(), false);
    addTool(tr("附加檔案"), Tool::Attachment, QStringLiteral("annot.attachment"), QKeySequence(),
            false);
    addTool(tr("量測距離"), Tool::Measure, QStringLiteral("annot.measure"), QKeySequence(), false);

    // 儲存那一組（ADR-008）。這個架構下每一次修改都當下就寫進磁碟，
    // 所以沒有「未存檔的修改」這個狀態——三顆按鈕的語意見 ADR。
    fileMenu->addSeparator();
    auto* saveAction = fileMenu->addAction(tr("儲存(&S)"));
    saveAction->setShortcut(QKeySequence::Save);
    registerRibbonAction(QStringLiteral("file.save"), saveAction);
    connect(saveAction, &QAction::triggered, this, [this] {
        if (currentPath_.isEmpty() || !controller_->isOpen()) return;
        // 按下 Ctrl+S 時使用者心裡的問題是「我的東西留住了嗎」。這裡的答案
        // 是肯定的，而唯一的回饋就是這句話——不說的話按下去像什麼都沒發生。
        controller_->runAutosaveIfDue();
        statusBar()->showMessage(tr("文件的修改已即時寫入磁碟"), 4000);
    });

    auto* saveAsAction = fileMenu->addAction(tr("另存新檔(&A)..."));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    registerRibbonAction(QStringLiteral("file.saveAs"), saveAsAction);
    connect(saveAsAction, &QAction::triggered, this, [this] { saveDocumentAs(); });

    auto* revertAction = fileMenu->addAction(tr("復原所有修改(&V)"));
    registerRibbonAction(QStringLiteral("file.revert"), revertAction);
    connect(revertAction, &QAction::triggered, this, [this] { revertAllChanges(); });

    fileMenu->addSeparator();
    auto* preferencesAction = fileMenu->addAction(tr("偏好設定(&P)..."));
    registerRibbonAction(QStringLiteral("app.preferences"), preferencesAction);
    preferencesAction->setShortcut(QKeySequence::Preferences);
    connect(preferencesAction, &QAction::triggered, this, [this] {
        PreferencesDialog dialog(settings_, &shortcuts_, this);
        // 鍵位即時套用並存檔。等按下確定才套的話，使用者在對話框裡改完一個鍵、
        // 按取消，會以為改動被撤銷——但鍵位表已經在記憶體裡改掉了，兩者對不上。
        connect(&dialog, &PreferencesDialog::shortcutsChanged, this, [this] {
            applyShortcutScheme();
            settings_->setShortcutsJson(shortcuts_.exportToJson());
        });
        if (dialog.exec() == QDialog::Accepted) {
            const bool localeChanged = dialog.localeChanged();
            dialog.apply();
            applySettings();
            restoreSession();
            if (localeChanged) {
                QMessageBox::information(
                    this, tr("介面語言"),
                    tr("語言設定已儲存，重新啟動 Alioth 後生效。"));
            }
        }
    });

    fileMenu->addSeparator();
    // 表單選單（PRD-FORM-001~）。
    //
    // 選單只切換「接下來要畫哪一種欄位」，真正的建立發生在使用者框出矩形
    // 之後——欄位的位置與大小是它的一部分，用對話框輸入座標沒有人會用。
    auto* formMenu = menuBar()->addMenu(tr("表單(&R)"));
    const auto addFieldTool = [&](const QString& text,
                                  engine::formbuild::BuildFieldType type, const QString& id) {
        auto* action = formMenu->addAction(text);
        action->setCheckable(true);
        toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, type] {
            pendingFieldType_ = type;
            pageView_->setTool(Tool::FormField);
        });
        registerRibbonAction(id, action);
        return action;
    };
    addFieldTool(tr("文字欄位"), engine::formbuild::BuildFieldType::Text,
                 QStringLiteral("form.textField"));
    addFieldTool(tr("核取方塊"), engine::formbuild::BuildFieldType::CheckBox,
                 QStringLiteral("form.checkBox"));
    addFieldTool(tr("選項按鈕"), engine::formbuild::BuildFieldType::RadioGroup,
                 QStringLiteral("form.radioButton"));
    addFieldTool(tr("下拉方塊"), engine::formbuild::BuildFieldType::ComboBox,
                 QStringLiteral("form.comboBox"));
    addFieldTool(tr("清單方塊"), engine::formbuild::BuildFieldType::ListBox,
                 QStringLiteral("form.listBox"));
    addFieldTool(tr("按鈕"), engine::formbuild::BuildFieldType::PushButton,
                 QStringLiteral("form.pushButton"));
    addFieldTool(tr("簽名欄位"), engine::formbuild::BuildFieldType::Signature,
                 QStringLiteral("form.signatureField"));

    // 表單資料（PRD-FORM-002）。匯出／匯入／重設都走 FormController 的
    // 非同步佇列——PDFium 的表單環境非執行緒安全，回呼在那條執行緒上，
    // 所以每個回呼都要排回 GUI 執行緒才能碰 widget。
    formMenu->addSeparator();
    auto* exportDataAction = formMenu->addAction(tr("匯出表單資料..."));
    registerRibbonAction(QStringLiteral("form.exportData"), exportDataAction);
    connect(exportDataAction, &QAction::triggered, this, [this] { exportFormData(); });

    auto* importDataAction = formMenu->addAction(tr("匯入表單資料..."));
    registerRibbonAction(QStringLiteral("form.importData"), importDataAction);
    connect(importDataAction, &QAction::triggered, this, [this] { importFormData(); });

    auto* resetFormAction = formMenu->addAction(tr("重設表單"));
    registerRibbonAction(QStringLiteral("form.resetForm"), resetFormAction);
    connect(resetFormAction, &QAction::triggered, this, [this] { resetFormFields(); });

    formMenu->addSeparator();
    auto* highlightFieldsAction = formMenu->addAction(tr("標示表單欄位"));
    highlightFieldsAction->setCheckable(true);
    registerRibbonAction(QStringLiteral("form.highlightFields"), highlightFieldsAction);
    connect(highlightFieldsAction, &QAction::toggled, this, [this](bool on) {
        // 欄位標示是檢視層的疊加，不改文件——所以不進命令堆疊，也不需要
        // 任何寫入權限。
        if (fieldsDock_ != nullptr && on) {
            fieldsDock_->show();
            fieldsDock_->raise();
        }
        pageView_->setFormFieldHighlight(on);
    });

    // 保護選單（PRD-ANN-032 / PRD-ANN-033）。
    //
    // 標記與套用刻意是兩個分開的動作，不是一個帶確認的動作：標記可逆
    // （只是一則 /Redact 註解，刪掉就回到原狀），套用不可逆（內容真的被刪掉）。
    // 合併成一個動作的話，使用者一次按錯就永久刪掉了內容。
    // 比較文件（PRD-CMP-001）。放在「檢視」而不是「組織」：它不修改任何東西，
    // 而「組織」底下的每一項都會改寫檔案。
    auto* compareAction = viewMenu->addAction(tr("比較文件(&D)..."));
    registerRibbonAction(QStringLiteral("document.compare"), compareAction);
    connect(compareAction, &QAction::triggered, this, [this] { compareWithDocument(); });

    auto* protectMenu = menuBar()->addMenu(tr("保護(&T)"));

    // 清除隱藏中繼資料（PRD-ANN-034）。與塗黑刻意分開：使用者常常只想在
    // 對外發布前清乾淨中繼資料，而文件裡一個字都不用塗黑。
    auto* sanitizeAction = protectMenu->addAction(tr("清除隱藏資訊(&M)..."));
    registerRibbonAction(QStringLiteral("protect.sanitize"), sanitizeAction);
    connect(sanitizeAction, &QAction::triggered, this, [this] { sanitizeDocument(); });

    protectMenu->addSeparator();
    auto* clearRedactAction = protectMenu->addAction(tr("移除所有塗黑標記"));
    registerRibbonAction(QStringLiteral("protect.redactClear"), clearRedactAction);
    connect(clearRedactAction, &QAction::triggered, this, [this] { clearRedactionMarks(); });

    protectMenu->addSeparator();
    auto* signAction = protectMenu->addAction(tr("數位簽署(&S)..."));
    registerRibbonAction(QStringLiteral("sign.digitalSign"), signAction);
    connect(signAction, &QAction::triggered, this, [this] { signCurrentDocument(); });

    auto* validateAction = protectMenu->addAction(tr("驗證簽章(&V)"));
    registerRibbonAction(QStringLiteral("sign.validate"), validateAction);
    connect(validateAction, &QAction::triggered, this, [this] {
        if (currentPath_.isEmpty() || !controller_->isOpen()) return;
        if (signatureDock_ != nullptr) {
            // 驗證結果只在簽章面板看得到，所以順手把它打開——把動作做成
            // 「按了以後什麼都沒發生」是這個功能最容易被當成壞掉的方式。
            signatureDock_->show();
            signatureDock_->raise();
        }
        signatures_->verify();
        statusBar()->showMessage(tr("驗證中…"), 3000);
    });

    protectMenu->addSeparator();
    auto* applyRedactAction = protectMenu->addAction(tr("套用塗黑（不可復原）..."));
    registerRibbonAction(QStringLiteral("protect.redactApply"), applyRedactAction);
    connect(applyRedactAction, &QAction::triggered, this, [this] { applyRedactionMarks(); });

    // 說明選單（PRD-UI-002 的 help.* 群組）。
    //
    // 授權那一項不是裝飾：Qt 走 LGPL v3 動態連結，隨附的思源黑體走 OFL，
    // 兩者都要求在產品裡提供授權聲明與取得原始碼／字型的方式。沒有這個入口
    // 就不符合授權條件，而那不會有任何測試或建置錯誤提醒。
    auto* helpMenu = menuBar()->addMenu(tr("說明(&H)"));
    auto* shortcutsAction = helpMenu->addAction(tr("鍵盤快速鍵(&K)"));
    registerRibbonAction(QStringLiteral("help.shortcuts"), shortcutsAction);
    connect(shortcutsAction, &QAction::triggered, this, [this] {
        QStringList lines;
        for (QAction* action : findChildren<QAction*>()) {
            if (action->shortcut().isEmpty() || action->text().isEmpty()) continue;
            QString label = action->text();
            label.remove(QLatin1Char('&'));
            lines << QStringLiteral("%1\t%2").arg(
                action->shortcut().toString(QKeySequence::NativeText), label);
        }
        lines.sort();
        lines.removeDuplicates();
        QMessageBox box(this);
        box.setWindowTitle(tr("鍵盤快速鍵"));
        box.setText(tr("目前已指派 %1 個快速鍵。").arg(lines.size()));
        // 明細放在 detailedText：一次列幾十行會把對話框撐出螢幕，
        // 而使用者多半只是想確認某一個鍵。
        box.setDetailedText(lines.join(QLatin1Char('\n')));
        box.exec();
    });

    auto* licenseAction = helpMenu->addAction(tr("授權聲明(&L)"));
    registerRibbonAction(QStringLiteral("help.license"), licenseAction);
    connect(licenseAction, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this, tr("授權聲明"),
            tr("Alioth 使用下列第三方元件：\n\n"
               "Qt 6（LGPL v3，動態連結）\n"
               "  本產品未修改 Qt，且以動態連結方式使用；您可以自行替換隨附的 Qt 動態庫。\n"
               "  原始碼取得：https://download.qt.io/\n\n"
               "PDFium（BSD 3-Clause）\n"
               "  已停用 JavaScript（V8）與 XFA。\n\n"
               "OpenSSL 3.x（Apache License 2.0）\n\n"
               "思源黑體 Noto Sans TC（SIL Open Font License 1.1）\n"
               "  完整授權條文隨附於安裝目錄的 OFL.txt。"));
    });

    helpMenu->addSeparator();
    auto* aboutAction = helpMenu->addAction(tr("關於 Alioth(&A)"));
    registerRibbonAction(QStringLiteral("help.about"), aboutAction);
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, tr("關於 Alioth"),
            tr("<b>Alioth</b> %1<br><br>跨平台專業 PDF 審閱工作站<br><br>"
               "Qt %2 · 建置於 %3<br><br>"
               "第三方元件的授權聲明見「說明 → 授權聲明」。")
                .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion()),
                     QString::fromLatin1(__DATE__)));
    });

    auto* quitAction = fileMenu->addAction(tr("結束(&X)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    auto* toolbar = addToolBar(tr("檢視"));
    toolbar->setObjectName(QStringLiteral("viewToolBar"));
    toolbar->setAccessibleName(tr("檢視工具列"));
    toolbar->setMovable(false);
    viewToolBar_ = toolbar;

    const auto addViewAction = [&](const QString& text, const QKeySequence& shortcut,
                                   auto&& handler) {
        auto* action = viewMenu->addAction(text);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, handler);
        toolbar->addAction(action);
        return action;
    };

    registerRibbonAction(QStringLiteral("view.zoomIn"),
                         addViewAction(tr("放大"), QKeySequence::ZoomIn,
                                       [this] { pageView_->zoomIn(); }));
    registerRibbonAction(QStringLiteral("view.zoomOut"),
                         addViewAction(tr("縮小"), QKeySequence::ZoomOut,
                                       [this] { pageView_->zoomOut(); }));
    registerRibbonAction(QStringLiteral("view.actualSize"),
                         addViewAction(tr("實際大小"), QKeySequence(Qt::CTRL | Qt::Key_0),
                                       [this] { pageView_->actualSize(); }));
    registerRibbonAction(QStringLiteral("view.fitPage"),
                         addViewAction(tr("符合頁面"), QKeySequence(Qt::CTRL | Qt::Key_1),
                                       [this] { pageView_->fitPage(); }));
    registerRibbonAction(QStringLiteral("view.fitVisible"),
                         addViewAction(tr("符合內容（忽略白邊）"),
                                       QKeySequence(Qt::CTRL | Qt::Key_3),
                                       [this] { fitVisible(); }));
    registerRibbonAction(QStringLiteral("view.fitWidth"),
                         addViewAction(tr("符合頁寬"), QKeySequence(Qt::CTRL | Qt::Key_2),
                                       [this] { pageView_->fitWidth(); }));

    // PRD-VIEW-007 的另一半：自訂背景與文字色。
    //
    // 與夜間模式並列而不是取代它：夜間模式是整幅反相（照片變負片），
    // 自訂配色只換接近白的底與接近黑的字，照片與圖表維持原色。
    // 兩者解決的是不同的問題，使用者也會分別想要。
    viewMenu->addSeparator();
    auto* customColorsAction = viewMenu->addAction(tr("自訂背景與文字色..."));
    registerRibbonAction(QStringLiteral("view.customColors"), customColorsAction);
    connect(customColorsAction, &QAction::triggered, this, [this] {
        const QColor background =
            QColorDialog::getColor(customBackground_, this, tr("背景色"));
        if (!background.isValid()) return;
        const QColor text = QColorDialog::getColor(customText_, this, tr("文字色"));
        if (!text.isValid()) return;
        customBackground_ = background;
        customText_ = text;
        controller_->setCustomColors(true, background, text);
        statusBar()->showMessage(tr("已套用自訂配色"), 4000);
    });

    auto* resetColorsAction = viewMenu->addAction(tr("回復原始色彩"));
    registerRibbonAction(QStringLiteral("view.resetColors"), resetColorsAction);
    connect(resetColorsAction, &QAction::triggered, this, [this] {
        controller_->setCustomColors(false, customBackground_, customText_);
        statusBar()->showMessage(tr("已回復原始色彩"), 4000);
    });

    // PRD-VIEW-015 / PRD-VIEW-016：尺規、參考線、格線與貼齊。
    //
    // 四個開關分開而不是合成一個「繪圖輔助」：使用者常常只要格線不要尺規，
    // 或只要貼齊不要看到格線（貼齊是行為，格線是視覺提示，兩者不必綁在一起）。
    viewMenu->addSeparator();
    auto* rulerAction = viewMenu->addAction(tr("尺規"));
    rulerAction->setCheckable(true);
    rulerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(rulerAction, &QAction::toggled, this, [this](bool on) {
        horizontalRuler_->setVisible(on);
        verticalRuler_->setVisible(on);
        if (rulerCorner_ != nullptr) rulerCorner_->setVisible(on);
        if (on) {
            horizontalRuler_->setMapping(pageView_->currentPageTransform(),
                                         pageView_->currentPageOriginInViewport().x());
            verticalRuler_->setMapping(pageView_->currentPageTransform(),
                                       pageView_->currentPageOriginInViewport().y());
        }
    });
    registerRibbonAction(QStringLiteral("view.rulers"), rulerAction);

    auto* guidesAction = viewMenu->addAction(tr("參考線"));
    guidesAction->setCheckable(true);
    connect(guidesAction, &QAction::toggled, this,
            [this](bool on) { pageView_->setGuidesVisible(on); });
    registerRibbonAction(QStringLiteral("view.guides"), guidesAction);

    auto* clearGuidesAction = viewMenu->addAction(tr("清除所有參考線"));
    connect(clearGuidesAction, &QAction::triggered, this, [this] {
        pageView_->guides().clear();
        pageView_->setGuidesVisible(pageView_->guidesVisible());
    });
    registerRibbonAction(QStringLiteral("view.clearGuides"), clearGuidesAction);

    auto* gridAction = viewMenu->addAction(tr("格線"));
    gridAction->setCheckable(true);
    connect(gridAction, &QAction::toggled, this, [this](bool on) {
        domain::GridSettings grid = pageView_->grid();
        grid.enabled = on;
        pageView_->setGrid(grid);
    });
    registerRibbonAction(QStringLiteral("view.grid"), gridAction);

    auto* snapAction = viewMenu->addAction(tr("貼齊格線與參考線"));
    snapAction->setCheckable(true);
    connect(snapAction, &QAction::toggled, this,
            [this](bool on) { pageView_->setSnapEnabled(on); });
    registerRibbonAction(QStringLiteral("view.snap"), snapAction);

    // PRD-UI-010 / PRD-UI-011：自訂 Ribbon 與快速存取列。
    //
    // 兩條需求指的是同一份資料——Ribbon 的分頁與快速存取列都描述在 ribbon::Layout
    // 裡，所以只有一個入口，不是兩個各改一半的對話框。
    viewMenu->addSeparator();
    auto* customizeRibbonAction = viewMenu->addAction(tr("自訂 Ribbon 與工具列..."));
    connect(customizeRibbonAction, &QAction::triggered, this, &MainWindow::customizeRibbon);
    registerRibbonAction(QStringLiteral("view.customizeRibbon"), customizeRibbonAction);

    // PRD-VIEW-009：全螢幕與簡報模式。狀態機在 domain/presentation.h，
    // 這裡只把它的三個狀態映射成實際的視窗與介面可見性。
    //
    // 兩者的差別是刻意的：全螢幕只是視窗佔滿螢幕，Ribbon 與面板都還在；
    // 簡報模式把一切介面藏掉，只留頁面內容。把兩者做成同一件事會讓
    // 「我只是想要大一點的視窗」的使用者失去所有工具列。
    viewMenu->addSeparator();
    auto* fullScreenAction = viewMenu->addAction(tr("全螢幕"));
    fullScreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    connect(fullScreenAction, &QAction::triggered, this, [this] {
        presentation_.toggleFullscreen();
        applyViewMode();
    });
    registerRibbonAction(QStringLiteral("view.fullScreen"), fullScreenAction);

    auto* presentationAction = viewMenu->addAction(tr("簡報模式"));
    presentationAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F11));
    connect(presentationAction, &QAction::triggered, this, [this] {
        presentation_.enterPresentation();
        applyViewMode();
    });
    registerRibbonAction(QStringLiteral("view.presentation"), presentationAction);

    // Esc 退出。用 QShortcut 而不是覆寫 keyPressEvent：焦點在面板裡的清單上時
    // 主視窗收不到按鍵，而使用者按 Esc 的時候焦點在哪裡是不確定的。
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::ApplicationShortcut);
    connect(escape, &QShortcut::activated, this, [this] {
        if (presentation_.handleEscape()) applyViewMode();
    });

    // 跳頁（PDF-XChange 的 View / Go To 群組）。
    //
    // 鍵盤本來就走得到（Home / End / PageUp / PageDown 由檢視區處理），
    // 但只有鍵盤走得到等於沒有：使用者要先知道有這回事。四個動作同時是
    // Ribbon 按鈕的接點，而且跟著文件開關啟用停用。
    viewMenu->addSeparator();
    const auto addGoTo = [&](const QString& text, const QString& id,
                             const QKeySequence& shortcut, auto&& resolvePage) {
        auto* action = viewMenu->addAction(text);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        registerRibbonAction(id, action);
        connect(action, &QAction::triggered, this,
                [this, resolvePage = std::forward<decltype(resolvePage)>(resolvePage)] {
                    if (!controller_->isOpen() || controller_->pageCount() <= 0) return;
                    const int target = resolvePage();
                    if (target < 0 || target >= controller_->pageCount()) return;
                    navigateToPage(target);
                });
        return action;
    };

    addGoTo(tr("第一頁"), QStringLiteral("nav.firstPage"),
            QKeySequence(Qt::CTRL | Qt::Key_Home), [] { return 0; });
    addGoTo(tr("上一頁"), QStringLiteral("nav.previousPage"),
            QKeySequence(Qt::CTRL | Qt::Key_PageUp),
            [this] { return pageView_->pageIndex() - 1; });
    addGoTo(tr("下一頁"), QStringLiteral("nav.nextPage"),
            QKeySequence(Qt::CTRL | Qt::Key_PageDown),
            [this] { return pageView_->pageIndex() + 1; });
    addGoTo(tr("最後一頁"), QStringLiteral("nav.lastPage"),
            QKeySequence(Qt::CTRL | Qt::Key_End),
            [this] { return controller_->pageCount() - 1; });

    // 指定頁碼。500 頁的文件靠捲動找第 317 頁是不可行的，而這是
    // PDF-XChange 的 Go To 群組裡使用頻率最高的一個。
    auto* goToPageAction = viewMenu->addAction(tr("跳至頁碼..."));
    goToPageAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    registerRibbonAction(QStringLiteral("nav.goToPage"), goToPageAction);
    connect(goToPageAction, &QAction::triggered, this, [this] {
        if (!controller_->isOpen() || controller_->pageCount() <= 0) {
            QMessageBox::information(this, tr("跳至頁碼"), tr("尚未開啟文件"));
            return;
        }
        bool accepted = false;
        const int page = QInputDialog::getInt(this, tr("跳至頁碼"), tr("頁碼："),
                                              pageView_->pageIndex() + 1, 1,
                                              controller_->pageCount(), 1, &accepted);
        if (!accepted) return;
        navigateToPage(page - 1);
    });

    // 瀏覽歷史前進後退（PRD-NAV-001）。
    //
    // Alt+方向鍵是瀏覽器與 Acrobat 共用的慣例，使用者不必學。
    // 兩個動作預設停用：沒有歷史時可以按但沒反應，比灰掉更讓人困惑。
    viewMenu->addSeparator();
    backAction_ = viewMenu->addAction(tr("上一個檢視位置"));
    backAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    backAction_->setEnabled(false);
    connect(backAction_, &QAction::triggered, this, [this] {
        const app::ViewPosition position = viewHistory_.goBack();
        pageView_->setPageIndex(position.pageIndex);
        if (position.scale > 0.0) pageView_->setScale(position.scale);
        updateNavigationActions();
    });
    registerRibbonAction(QStringLiteral("nav.back"), backAction_);

    forwardAction_ = viewMenu->addAction(tr("下一個檢視位置"));
    forwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    forwardAction_->setEnabled(false);
    connect(forwardAction_, &QAction::triggered, this, [this] {
        const app::ViewPosition position = viewHistory_.goForward();
        pageView_->setPageIndex(position.pageIndex);
        if (position.scale > 0.0) pageView_->setScale(position.scale);
        updateNavigationActions();
    });
    registerRibbonAction(QStringLiteral("nav.forward"), forwardAction_);

    viewMenu->addSeparator();
    auto* splitMenu = viewMenu->addMenu(tr("分割檢視"));
    auto* splitGroup = new QActionGroup(this);
    splitGroup->setExclusive(true);

    const auto addSplitMode = [&](const QString& text, SplitMode mode, bool checked) {
        auto* action = splitMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        splitGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setSplitMode(mode); });
        return action;
    };

    addSplitMode(tr("不分割"), SplitMode::None, true);
    registerRibbonAction(QStringLiteral("view.splitHorizontal"),
                         addSplitMode(tr("水平分割"), SplitMode::Horizontal, false));
    registerRibbonAction(QStringLiteral("view.splitVertical"),
                         addSplitMode(tr("垂直分割"), SplitMode::Vertical, false));
    // PRD-VIEW-017：試算表式分割（四格）。
    registerRibbonAction(QStringLiteral("view.splitQuad"),
                         addSplitMode(tr("試算表式分割（四格）"), SplitMode::Quad, false));

    viewMenu->addSeparator();
    auto* layoutMenu = viewMenu->addMenu(tr("版面模式"));
    auto* layoutGroup = new QActionGroup(this);
    layoutGroup->setExclusive(true);

    const auto addLayoutMode = [&](const QString& text, domain::LayoutMode mode, bool checked) {
        auto* action = layoutMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        layoutGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            domain::LayoutOptions options = pageView_->layoutOptions();
            options.mode = mode;
            pageView_->setLayoutOptions(options);
        });
        return action;
    };

    registerRibbonAction(QStringLiteral("view.singlePage"),
                         addLayoutMode(tr("單頁"), domain::LayoutMode::SinglePage, false));
    registerRibbonAction(QStringLiteral("view.continuous"),
                         addLayoutMode(tr("連續"), domain::LayoutMode::Continuous, true));
    registerRibbonAction(QStringLiteral("view.twoPage"),
                         addLayoutMode(tr("雙頁"), domain::LayoutMode::TwoPage, false));
    addLayoutMode(tr("雙頁連續"), domain::LayoutMode::TwoPageContinuous, false);
    // PRD-VIEW-011：頁面左右排列（Ribbon Layout）。版面計算與 RTL 反向早就完成
    // 並有往返測試，缺的只是選單入口。
    registerRibbonAction(QStringLiteral("view.horizontal"),
                         addLayoutMode(tr("左右排列"), domain::LayoutMode::Horizontal, false));

    layoutMenu->addSeparator();
    auto* coverAction = layoutMenu->addAction(tr("封面獨立"));
    registerRibbonAction(QStringLiteral("view.coverPage"), coverAction);
    coverAction->setCheckable(true);
    coverAction->setChecked(true);
    connect(coverAction, &QAction::toggled, this, [this](bool on) {
        domain::LayoutOptions options = pageView_->layoutOptions();
        options.coverPageSeparate = on;
        pageView_->setLayoutOptions(options);
    });

    auto* rtlAction = layoutMenu->addAction(tr("右至左版面"));
    rtlAction->setCheckable(true);
    connect(rtlAction, &QAction::toggled, this, [this](bool on) {
        domain::LayoutOptions options = pageView_->layoutOptions();
        options.rightToLeft = on;
        pageView_->setLayoutOptions(options);
    });

    viewMenu->addSeparator();
    auto* nightAction = viewMenu->addAction(tr("夜間模式"));
    nightAction->setCheckable(true);
    connect(nightAction, &QAction::toggled, this,
            [this](bool on) { controller_->setNightMode(on); });

    auto* qualityMenu = viewMenu->addMenu(tr("顯示品質"));
    auto* grayAction = qualityMenu->addAction(tr("灰階預覽"));
    auto* pathsAction = qualityMenu->addAction(tr("平滑線條"));
    auto* textAction = qualityMenu->addAction(tr("平滑文字"));
    auto* imagesAction = qualityMenu->addAction(tr("平滑影像"));
    const auto& quality = controller_->renderOptions();
    const QList<QAction*> qualityActions{grayAction, pathsAction, textAction, imagesAction};
    for (auto* action : qualityActions) action->setCheckable(true);
    grayAction->setChecked(quality.grayscale);
    pathsAction->setChecked(!quality.strokeAdjust);
    textAction->setChecked(quality.smoothText);
    imagesAction->setChecked(quality.smoothImages);
    registerRibbonAction(QStringLiteral("view.grayscale"), grayAction);
    registerRibbonAction(QStringLiteral("view.smoothPaths"), pathsAction);
    registerRibbonAction(QStringLiteral("view.smoothText"), textAction);
    registerRibbonAction(QStringLiteral("view.smoothImages"), imagesAction);
    for (auto* action : qualityActions) {
        connect(action, &QAction::toggled, this,
                [this, grayAction, pathsAction, textAction, imagesAction] {
            controller_->setRenderQuality(grayAction->isChecked(), pathsAction->isChecked(),
                                           textAction->isChecked(), imagesAction->isChecked());
        });
    }

    // PRD-VIEW-018：透明度格線。純檢視選項，不改文件——關掉之後畫面應該
    // 跟從沒開過一樣，這是它跟夜間模式（會反轉實際渲染出來的像素）唯一
    // 不同但同樣重要的地方。
    auto* transparencyGridAction = viewMenu->addAction(tr("透明度格線"));
    transparencyGridAction->setCheckable(true);
    connect(transparencyGridAction, &QAction::toggled, this,
            [this](bool on) { controller_->setTransparencyGrid(on); });

    auto* rotateAction = viewMenu->addAction(tr("向右旋轉檢視"));
    registerRibbonAction(QStringLiteral("view.rotateClockwise"), rotateAction);
    rotateAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus));
    connect(rotateAction, &QAction::triggered, this, [this] {
        pageView_->setRotation(domain::addRotation(pageView_->rotation(), domain::Rotation::Cw90));
    });

    auto* rotateBackAction = viewMenu->addAction(tr("向左旋轉檢視"));
    registerRibbonAction(QStringLiteral("view.rotateCounterClockwise"), rotateBackAction);
    rotateBackAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Minus));
    connect(rotateBackAction, &QAction::triggered, this, [this] {
        pageView_->setRotation(domain::addRotation(pageView_->rotation(), domain::Rotation::Cw270));
    });

    // 朗讀（PRD-A11Y-006）。內容一律取自結構樹的閱讀順序（與 Order／Tags 面板
    // 同一份資料），因此準不準取決於文件有沒有標籤——這件事在動作的
    // tooltip／狀態列訊息裡要說清楚，不能讓使用者以為朗讀跳字是程式的錯。
    viewMenu->addSeparator();
    auto* readAloudAction = viewMenu->addAction(tr("朗讀目前頁面(&U)"));
    registerRibbonAction(QStringLiteral("view.readAloudToggle"), readAloudAction);
    readAloudAction->setCheckable(true);
    readAloudAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    connect(readAloudAction, &QAction::toggled, this, [this, readAloudAction](bool checked) {
        if (!checked) {
            readAloud_->stop();
            return;
        }
        if (currentPath_.isEmpty() || !controller_->isOpen()) {
            statusBar()->showMessage(tr("尚未開啟文件"), 5000);
            readAloudAction->blockSignals(true);
            readAloudAction->setChecked(false);
            readAloudAction->blockSignals(false);
            return;
        }
        readAloud_->start(tagsPanel_->structTree(), pageView_->pageIndex(),
                          controller_->pageCount());
    });
    connect(readAloud_, &app::ReadAloudController::pageChanged, this,
            [this](int pageIndex) { navigateToPage(pageIndex); });
    connect(readAloud_, &app::ReadAloudController::finished, this, [this, readAloudAction] {
        readAloudAction->setChecked(false);
        statusBar()->showMessage(tr("朗讀結束"), 5000);
    });
    connect(readAloud_, &app::ReadAloudController::unavailable, this,
            [this, readAloudAction](const QString& reason) {
                readAloudAction->setChecked(false);
                statusBar()->showMessage(tr("無法朗讀：%1").arg(reason), 8000);
            });

    auto* readAloudPauseAction = viewMenu->addAction(tr("暫停／繼續朗讀(&Z)"));
    registerRibbonAction(QStringLiteral("view.readAloudPause"), readAloudPauseAction);
    readAloudPauseAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(readAloudPauseAction, &QAction::triggered, this, [this] {
        if (readAloud_->isActive()) readAloud_->togglePause();
    });
}

void MainWindow::buildDockPanels() {
    // PRD-UI-003 要求 12 個可停靠面板。縮圖與書籤已接上真實資料，
    // 其餘面板先立框架，內容由對應 WBS 填。
    // id 是給 UIA 的 AutomationId 與 QMainWindow::saveState 用的穩定識別碼，
    // 必須是 ASCII 且不隨語系變動；title 是給人看的，會被翻譯。
    // 兩者混用（例如拿中文標題當 objectName）會讓版面在切換語言後還原失敗，
    // 而那個症狀是「面板位置偶爾自己跑掉」，極難追。
    const auto addPanel = [this](const QString& id, const QString& title, Qt::DockWidgetArea area,
                                 QWidget* content) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(id);
        // QDockWidget 的可及性名稱不會自動從 windowTitle 推導，
        // 螢幕閱讀器在 F6 循環到這個面板時需要它才念得出面板名稱。
        dock->setAccessibleName(title);
        dock->setWidget(content);
        if (content != nullptr && content->objectName().isEmpty()) {
            content->setObjectName(id + QStringLiteral("Content"));
        }
        addDockWidget(area, dock);
        return dock;
    };

    thumbnailList_ = new QListWidget(this);
    thumbnailList_->setObjectName(QStringLiteral("thumbnailList"));
    thumbnailList_->setAccessibleName(tr("頁面縮圖"));
    thumbnailList_->setAccessibleDescription(tr("方向鍵選頁，Enter 跳到該頁"));
    thumbnailList_->setViewMode(QListView::IconMode);
    thumbnailList_->setIconSize(QSize(120, 160));
    thumbnailList_->setResizeMode(QListView::Adjust);
    thumbnailList_->setMovement(QListView::Static);
    thumbnailList_->setSpacing(6);
    // 拖曳重排（PRD-NAV-004）。InternalMove 讓 Qt 負責拖放的視覺回饋，
    // 我們只在放開時把「從哪到哪」換成一次頁面移動操作。
    thumbnailList_->setDragDropMode(QAbstractItemView::InternalMove);
    thumbnailList_->setDefaultDropAction(Qt::MoveAction);

    connect(thumbnailList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && !reorderingThumbnails_) navigateToPage(row);
    });

    // QListWidget 在內部搬移之後才發出 rowsMoved。用它而不是攔截 dropEvent：
    // 攔截 drop 得自己算出目標索引，而 Qt 的計算會因為 IconMode 的排列方向
    // 與捲動位置而不同——自己算一份必然會在某個情況下與畫面看到的不一致。
    connect(thumbnailList_->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int start, int end, const QModelIndex&, int destination) {
                if (currentPath_.isEmpty() || reorderingThumbnails_) return;
                if (start != end) return;  // 目前一次只拖一頁

                // Qt 的 destination 是「移除來源之前」的插入點。往後拖時要減一，
                // 否則頁面會落在使用者放開的位置後面一格。
                const int target = destination > start ? destination - 1 : destination;
                if (target == start) return;

                // 先把清單還原成文件目前的樣子：真正的重排由重新載入後的
                // populateThumbnails() 呈現。少了這一步，操作失敗時清單會停在
                // 使用者拖過的位置上，而檔案根本沒變。
                reorderingThumbnails_ = true;
                const std::vector<int> pages{start};
                commitPageOperation(tr("移動頁面"), [this, pages, target] {
                    return pageOps_->movePages(currentPath_, pages, target,
                                               app::RewriteConsent::confirmed());
                });
                reorderingThumbnails_ = false;
                populateThumbnails();
            });

    outlineTree_ = new QTreeWidget(this);
    outlineTree_->setObjectName(QStringLiteral("outlineTree"));
    outlineTree_->setAccessibleName(tr("書籤"));
    outlineTree_->setAccessibleDescription(tr("方向鍵展開與收合，Enter 跳到書籤所指位置"));
    outlineTree_->setHeaderHidden(true);
    connect(outlineTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        const QVariant page = item->data(0, Qt::UserRole);
        if (page.isValid()) navigateToPage(page.toInt());
    });
    connect(outlineTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        const QVariant page = item->data(0, Qt::UserRole);
        if (page.isValid()) navigateToPage(page.toInt());
    });

    thumbnailDock_ = addPanel(QStringLiteral("thumbnailDock"), tr("縮圖"), Qt::LeftDockWidgetArea, thumbnailList_);
    bookmarkDock_ = addPanel(QStringLiteral("bookmarkDock"), tr("書籤"), Qt::LeftDockWidgetArea, outlineTree_);
    tabifyDockWidget(thumbnailDock_, bookmarkDock_);
    thumbnailDock_->raise();

    // 搜尋面板（PRD-UI-003 / PRD-SRCH-001）。結果逐頁進來，不等整份掃完。
    auto* searchPanel = new QWidget(this);
    auto* searchLayout = new QVBoxLayout(searchPanel);
    searchLayout->setContentsMargins(4, 4, 4, 4);
    searchField_ = new QLineEdit(searchPanel);
    searchField_->setObjectName(QStringLiteral("searchField"));
    // placeholderText 不能當可及性名稱：使用者一開始打字它就消失，
    // 螢幕閱讀器隨即失去這個欄位的用途說明。
    searchField_->setAccessibleName(tr("搜尋文件內容"));
    searchField_->setPlaceholderText(tr("搜尋文件內容"));
    searchResults_ = new QListWidget(searchPanel);
    searchResults_->setObjectName(QStringLiteral("searchResults"));
    searchResults_->setAccessibleName(tr("搜尋結果"));
    searchLayout->addWidget(searchField_);
    searchLayout->addWidget(searchResults_);

    connect(searchField_, &QLineEdit::returnPressed, this, [this] { runSearch(); });
    connect(searchResults_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        selection_->selectSearchHit(static_cast<std::size_t>(row));
        const auto& hits = selection_->searchHits();
        if (row < static_cast<int>(hits.size())) {
            navigateToPage(hits[static_cast<std::size_t>(row)].pageIndex);
        }
    });

    searchDock_ = addPanel(QStringLiteral("searchDock"), tr("搜尋"), Qt::RightDockWidgetArea, searchPanel);
    QDockWidget* search = searchDock_;
    // 註解列表（PRD-ANN-008）：清單加上篩選與排序列。
    auto* commentPanel = new QWidget(this);
    auto* commentLayout = new QVBoxLayout(commentPanel);
    commentLayout->setContentsMargins(4, 4, 4, 4);

    auto* filterRow = new QWidget(commentPanel);
    auto* filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);

    annotationAuthorFilter_ = new QComboBox(filterRow);
    // objectName 是 UIA 的 AutomationId 來源。少了它，自動化測試與螢幕閱讀器
    // 都只看得到一個匿名下拉——本專案的無障礙稽核閘門會直接擋下來。
    annotationAuthorFilter_->setObjectName(QStringLiteral("annotationAuthorFilter"));
    annotationAuthorFilter_->setAccessibleName(tr("依作者篩選"));
    annotationTypeFilter_ = new QComboBox(filterRow);
    annotationTypeFilter_->setObjectName(QStringLiteral("annotationTypeFilter"));
    annotationTypeFilter_->setAccessibleName(tr("依類型篩選"));
    annotationSortKey_ = new QComboBox(filterRow);
    annotationSortKey_->setObjectName(QStringLiteral("annotationSortKey"));
    annotationSortKey_->setAccessibleName(tr("排序方式"));
    annotationSortKey_->addItem(tr("依頁碼"), static_cast<int>(domain::AnnotationSortKey::Page));
    annotationSortKey_->addItem(tr("依作者"), static_cast<int>(domain::AnnotationSortKey::Author));
    annotationSortKey_->addItem(tr("依類型"), static_cast<int>(domain::AnnotationSortKey::Type));
    annotationSortKey_->addItem(tr("依日期"), static_cast<int>(domain::AnnotationSortKey::Date));
    filterLayout->addWidget(annotationAuthorFilter_);
    filterLayout->addWidget(annotationTypeFilter_);
    filterLayout->addWidget(annotationSortKey_);
    commentLayout->addWidget(filterRow);

    annotationSearch_ = new QLineEdit(commentPanel);
    annotationSearch_->setObjectName(QStringLiteral("annotationSearch"));
    annotationSearch_->setAccessibleName(tr("在註解中尋找"));
    annotationSearch_->setPlaceholderText(tr("在註解中尋找"));
    commentLayout->addWidget(annotationSearch_);

    annotationList_ = new QListWidget(commentPanel);
    annotationList_->setObjectName(QStringLiteral("annotationList"));
    annotationList_->setAccessibleName(tr("註解清單"));
    // 可多選（PRD-ANN-013「匯出選定註解」）。仍然有「目前這一列」的概念，
    // 所以跳頁與雙擊開啟註釋視窗的行為完全不變。
    annotationList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    commentLayout->addWidget(annotationList_, 1);

    // 點選跳頁時查的是**篩選後那一列對應的原始索引**，不是清單列號。
    // 直接拿列號去索引原陣列，是「點了第 3 列卻跳到第 7 則」的來源。
    connect(annotationList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= static_cast<int>(annotationOrder_.size())) {
            selectedAnnotation_ = static_cast<std::size_t>(-1);
            if (pageView_ != nullptr) pageView_->clearHighlightedRect();
            return;
        }
        const auto& items = controller_->annotations();
        const std::size_t index = annotationOrder_[static_cast<std::size_t>(row)];
        if (index >= items.size()) return;
        // 清單與頁面上的選取是同一件事的兩個視圖：在清單上點一列，頁面上
        // 那一則也要框起來，否則使用者得自己在頁面上找它在哪。
        // 這裡直接寫欄位而不是呼叫 selectAnnotation()，避免它再回頭設一次
        // 目前列——那會讓這個處理函式看起來像會遞迴。
        selectedAnnotation_ = index;
        if (pageView_ != nullptr) {
            pageView_->setHighlightedRect(items[index].pageIndex, items[index].rect);
        }
        navigateToPage(items[index].pageIndex);
    });

    // 雙擊清單開啟註釋視窗（PRD-ANN-004 的「雙向連結」之一）。用雙擊而不是單擊：
    // 單擊已經是「跳到那一頁」，再讓它同時彈出一個視窗，使用者只是想瀏覽清單
    // 時會被一連串視窗打斷。
    connect(annotationList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                const int row = annotationList_->row(item);
                if (row < 0 || row >= static_cast<int>(annotationOrder_.size())) return;
                openNotePopup(annotationOrder_[static_cast<std::size_t>(row)]);
            });

    // 刪除註解（PRD-ANN-010）。Delete 鍵作用在清單上而不是整個視窗：
    // 焦點在頁面上時按 Delete 的意圖是刪掉選取的註解，那是另一條路徑；
    // 掛在視窗層級會讓兩者互搶。
    // 複製／貼上註解（PRD-ANN-011）。與刪除一樣掛在清單上而不是視窗層級：
    // 焦點在頁面上時 Ctrl+C 的意圖是複製選取的文字，那是另一條路徑。
    auto* copyAnnotationAction = new QAction(tr("複製註解"), annotationList_);
    copyAnnotationAction->setShortcut(QKeySequence::Copy);
    copyAnnotationAction->setShortcutContext(Qt::WidgetShortcut);
    annotationList_->addAction(copyAnnotationAction);
    connect(copyAnnotationAction, &QAction::triggered, this,
            [this] { copySelectedAnnotation(); });
    registerRibbonAction(QStringLiteral("comment.copy"), copyAnnotationAction);

    auto* pasteAnnotationAction = new QAction(tr("貼上註解"), annotationList_);
    pasteAnnotationAction->setShortcut(QKeySequence::Paste);
    pasteAnnotationAction->setShortcutContext(Qt::WidgetShortcut);
    annotationList_->addAction(pasteAnnotationAction);
    connect(pasteAnnotationAction, &QAction::triggered, this, [this] { pasteAnnotations(); });
    registerRibbonAction(QStringLiteral("comment.paste"), pasteAnnotationAction);

    // 回覆與審閱狀態（PRD-ANN-007）。兩者共用同一條寫入路徑，因為在 PDF 裡
    // 它們是同一件事：狀態由一則獨立的回覆註解承載，而不是改寫原註解——
    // 那樣「誰在什麼時候標成已完成」才留得下來。
    auto* replyAction = new QAction(tr("回覆..."), annotationList_);
    annotationList_->addAction(replyAction);
    connect(replyAction, &QAction::triggered, this, [this] { replyToSelectedAnnotation({}); });
    registerRibbonAction(QStringLiteral("comment.reply"), replyAction);

    auto* statusMenu = new QMenu(tr("設定狀態"), annotationList_);
    const auto addStatus = [&](const QString& text, const QString& state) {
        QAction* action = statusMenu->addAction(text);
        connect(action, &QAction::triggered, this,
                [this, state] { replyToSelectedAnnotation(state); });
        return action;
    };
    addStatus(tr("已接受"), QStringLiteral("Accepted"));
    addStatus(tr("已拒絕"), QStringLiteral("Rejected"));
    addStatus(tr("已完成"), QStringLiteral("Completed"));
    addStatus(tr("已取消"), QStringLiteral("Cancelled"));
    annotationList_->addAction(statusMenu->menuAction());
    registerRibbonAction(QStringLiteral("comment.setStatus"), statusMenu->menuAction());

    auto* deleteAnnotationAction = new QAction(tr("刪除註解"), annotationList_);
    deleteAnnotationAction->setShortcut(QKeySequence::Delete);
    deleteAnnotationAction->setShortcutContext(Qt::WidgetShortcut);
    annotationList_->addAction(deleteAnnotationAction);
    annotationList_->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(deleteAnnotationAction, &QAction::triggered, this,
            [this] { deleteSelectedAnnotation(); });
    registerRibbonAction(QStringLiteral("comment.delete"), deleteAnnotationAction);

    connect(annotationAuthorFilter_, &QComboBox::currentIndexChanged, this,
            [this](int) { populateAnnotations(); });
    connect(annotationTypeFilter_, &QComboBox::currentIndexChanged, this,
            [this](int) { populateAnnotations(); });
    connect(annotationSortKey_, &QComboBox::currentIndexChanged, this,
            [this](int) { populateAnnotations(); });
    connect(annotationSearch_, &QLineEdit::textChanged, this,
            [this](const QString&) { populateAnnotations(); });

    signaturePanel_ = new SignaturePanel(signatures_, this);
    signatureDock_ = addPanel(QStringLiteral("signatureDock"), tr("簽章"), Qt::LeftDockWidgetArea, signaturePanel_);

    // commentDock_ 與 searchDock_ 先前沒有被指派，buildPanelActions 的 bindPanel
    // 因此靜默跳過這兩個面板——Ribbon 上的開關按了沒反應。
    commentDock_ = addPanel(QStringLiteral("commentDock"), tr("註解"), Qt::RightDockWidgetArea, commentPanel);
    QDockWidget* comments = commentDock_;
    tabifyDockWidget(search, comments);
    auto* propertiesList = new QListWidget(this);
    propertiesList->setObjectName(QStringLiteral("propertiesList"));
    propertiesList->setAccessibleName(tr("文件屬性"));
    auto* properties = addPanel(QStringLiteral("propertiesDock"), tr("屬性"), Qt::RightDockWidgetArea, propertiesList);
    tabifyDockWidget(comments, properties);

    historyPanel_ = new HistoryPanel(&history_, this);
    historyDock_ = addPanel(QStringLiteral("historyDock"), tr("開啟記錄"), Qt::LeftDockWidgetArea, historyPanel_);
    tabifyDockWidget(bookmarkDock_, historyDock_);
    connect(historyPanel_, &HistoryPanel::openRequested, this,
            [this](const QString& path, int pageIndex, double scale) {
                openPath(path);
                pendingRestorePage_ = pageIndex;
                pendingRestoreScale_ = scale;
            });
    connect(historyPanel_, &HistoryPanel::markRequested, this, [this](const QString& path,
                                                                     int pageIndex) {
        if (path != alioth::app::normalizeDocumentPath(currentPath_)) openPath(path);
        pendingRestorePage_ = pageIndex;
    });

    destinationsPanel_ = new DestinationsPanel(&navigation_, this);
    destinationsDock_ = addPanel(QStringLiteral("destinationsDock"), tr("命名目標"), Qt::LeftDockWidgetArea, destinationsPanel_);
    tabifyDockWidget(bookmarkDock_, destinationsDock_);
    connect(destinationsPanel_, &DestinationsPanel::destinationActivated, this,
            [this](int pageIndex, bool) { navigateToPage(pageIndex); });

    linksPanel_ = new LinksPanel(controller_, this);
    linksDock_ = addPanel(QStringLiteral("linksDock"), tr("連結"), Qt::LeftDockWidgetArea, linksPanel_);
    tabifyDockWidget(bookmarkDock_, linksDock_);
    connect(controller_, &app::DocumentController::linksReady, linksPanel_,
            &LinksPanel::linksArrived);
    // 開啟走同一條確認流程（PRD §8.2）。面板自己開等於多一個繞過確認的側門。
    connect(linksPanel_, &LinksPanel::linkActivated, this,
            [this](const domain::LinkTarget& target) { followLink(target); });
    // 收起來的期間不跟著翻頁，所以打開的當下要補一次；否則面板會停在
    // 上次收起來時的那一頁，而使用者看不出那是舊資料。
    connect(linksDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (!visible || currentPath_.isEmpty()) return;
        linksPanel_->setPage(pageView_->pageIndex());
    });

    layersPanel_ = new LayersPanel(this);
    layersDock_ = addPanel(QStringLiteral("layersDock"), tr("圖層"), Qt::LeftDockWidgetArea, layersPanel_);
    tabifyDockWidget(bookmarkDock_, layersDock_);
    connect(controller_, &app::DocumentController::layersReady, this,
            [this] { layersPanel_->setTree(controller_->layers()); });
    connect(layersPanel_, &LayersPanel::layerVisibilityRequested, this,
            [this](int objectNumber, bool visible) {
                // 兩邊模型要一致。畫面不會跟著變——那是 ADR-003 記錄的降級，
                // 面板自己會顯示說明，這裡不再重複提示。
                controller_->setLayerVisible(objectNumber, visible);
            });

    fieldsPanel_ = new FieldsPanel(this);
    fieldsDock_ = addPanel(QStringLiteral("fieldsDock"), tr("欄位"), Qt::RightDockWidgetArea, fieldsPanel_);
    connect(forms_, &app::FormController::fieldsReady, this,
            [this] { fieldsPanel_->setFields(forms_->fields()); });
    connect(fieldsPanel_, &FieldsPanel::fieldActivated, this, [this](const QString&, int page) {
        if (page >= 0) navigateToPage(page);
    });

    propertiesPanel_ = new AnnotationPropertiesPanel(this);
    propertiesDock_ = addPanel(QStringLiteral("annotationPropertiesDock"), tr("註解屬性"), Qt::RightDockWidgetArea, propertiesPanel_);

    attachmentsPanel_ = new AttachmentsPanel(this);
    // Tags 面板（PRD-A11Y-001）。放在左側與書籤同一疊：兩者都是文件的
    // 導覽結構，使用者要比對「書籤有沒有反映真正的標題階層」時會來回切換。
    tagsPanel_ = new TagsPanel(this);
    tagsDock_ = addPanel(QStringLiteral("tagsDock"), tr("標籤"), Qt::LeftDockWidgetArea, tagsPanel_);
    tabifyDockWidget(bookmarkDock_, tagsDock_);
    connect(tagsPanel_, &TagsPanel::elementActivated, this,
            [this](int pageIndex) { navigateToPage(pageIndex); });
    connect(tagsPanel_, &TagsPanel::alternateTextEditRequested, this,
            &MainWindow::editAlternateText);

    // Order 面板（PRD-A11Y-002）：與 Tags 面板同一疊，兩者都是在看
    // 「同一份結構樹」，只是切成兩種問法（樹狀關係 vs. 逐頁順序比對）。
    orderPanel_ = new OrderPanel(this);
    orderDock_ = addPanel(QStringLiteral("orderDock"), tr("閱讀順序"), Qt::LeftDockWidgetArea, orderPanel_);
    tabifyDockWidget(bookmarkDock_, orderDock_);

    // 無障礙檢查器（PRD-A11Y-003）放右側，跟屬性／註解列表同一類「檢視結果」面板。
    inspectorPanel_ = new InspectorPanel(this);
    inspectorDock_ = addPanel(QStringLiteral("inspectorDock"), tr("無障礙檢查器"),
                              Qt::RightDockWidgetArea, inspectorPanel_);
    tabifyDockWidget(commentDock_, inspectorDock_);
    connect(inspectorPanel_, &InspectorPanel::findingActivated, this,
            [this](int pageIndex) { navigateToPage(pageIndex); });

    // Pan & Zoom（PRD-ZOOM-005）與放大鏡（PRD-ZOOM-004）。
    //
    // 兩者的邏輯核心早就完成並有往返測試，缺的只是一個入口。
    // 放在右側與檢視類面板同一疊；預設收起（見 applyDefaultPanelLayout）。
    panZoomPanel_ = new PanZoomPanel(controller_, this);
    panZoomDock_ = addPanel(QStringLiteral("panZoomDock"), tr("Pan & Zoom"),
                            Qt::RightDockWidgetArea, panZoomPanel_);
    connect(panZoomPanel_, &PanZoomPanel::viewportOriginRequested, this,
            [this](int pageIndex, const domain::PointF& origin) {
                if (pageIndex != pageView_->pageIndex()) pageView_->setPageIndex(pageIndex);
                pageView_->scrollToPointInCurrentPage(origin);
            });
    // 視框要跟著主視圖走。捲動、縮放、換頁三種都會改變它，缺一種就會出現
    // 「視框停在原地」——那比沒有這個面板更誤導。
    const auto syncPanZoom = [this] {
        if (panZoomDock_ == nullptr || !panZoomDock_->isVisible()) return;
        panZoomPanel_->setPage(pageView_->pageIndex());
        panZoomPanel_->setMainViewport(pageView_->visibleRectInCurrentPage(), pageView_->scale());
    };
    connect(pageView_, &PageView::viewportChanged, this, syncPanZoom);
    connect(pageView_, &PageView::scaleChanged, this, [syncPanZoom](double) { syncPanZoom(); });
    connect(pageView_, &PageView::pageChanged, this, [syncPanZoom](int) { syncPanZoom(); });
    // 面板被叫出來時也要立刻同步一次，否則在它顯示之前的那些訊號全被略過，
    // 使用者看到的是一個空白面板直到下一次捲動。
    connect(panZoomDock_, &QDockWidget::visibilityChanged, this,
            [syncPanZoom](bool visible) { if (visible) syncPanZoom(); });

    loupePanel_ = new LoupeWidget(controller_, this);
    loupeDock_ = addPanel(QStringLiteral("loupeDock"), tr("放大鏡"), Qt::RightDockWidgetArea,
                          loupePanel_);
    connect(pageView_, &PageView::cursorMoved, this,
            [this](int pageIndex, const alioth::domain::PointF& pagePoint) {
                updateGeometryLabel(pageIndex, &pagePoint);
                if (loupeDock_ == nullptr || !loupeDock_->isVisible()) return;
                loupePanel_->updateCursor(pageIndex, pagePoint, pageView_->scale());
            });

    attachmentsDock_ = addPanel(QStringLiteral("attachmentsDock"), tr("附件"), Qt::RightDockWidgetArea, attachmentsPanel_);
    tabifyDockWidget(comments, attachmentsDock_);
    tabifyDockWidget(comments, panZoomDock_);
    tabifyDockWidget(comments, loupeDock_);
    tabifyDockWidget(comments, fieldsDock_);
    tabifyDockWidget(comments, propertiesDock_);
    connect(attachmentsPanel_, &AttachmentsPanel::locateRequested, this,
            [this](int pageIndex) { navigateToPage(pageIndex); });
    connect(attachmentsPanel_, &AttachmentsPanel::saveRequested, this,
            [this](int index, const QString& suggested) { saveAttachment(index, suggested); });

    search->raise();
    thumbnailDock_->raise();

    applyDefaultPanelLayout();
}

void MainWindow::exportCurrentPageImage() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("匯出頁面為影像"), tr("尚未開啟文件"));
        return;
    }

    // 只列出這台機器真的裝得到外掛的格式。列出來卻寫不出去，使用者會以為
    // 是自己的檔名有問題——引擎那邊已經提供 isFormatSupported 就是為了這個。
    struct FormatEntry {
        engine::iosec::ImageExportFormat format;
        const char* filter;
        const char* suffix;
    };
    const FormatEntry entries[] = {
        {engine::iosec::ImageExportFormat::Png, QT_TR_NOOP("PNG 影像 (*.png)"), "png"},
        {engine::iosec::ImageExportFormat::Jpeg, QT_TR_NOOP("JPEG 影像 (*.jpg)"), "jpg"},
        {engine::iosec::ImageExportFormat::Tiff, QT_TR_NOOP("TIFF 影像 (*.tif)"), "tif"},
    };

    QStringList filters;
    std::vector<const FormatEntry*> available;
    for (const FormatEntry& entry : entries) {
        if (!engine::iosec::isFormatSupported(entry.format)) continue;
        filters << tr(entry.filter);
        available.push_back(&entry);
    }
    if (available.empty()) {
        QMessageBox::warning(this, tr("匯出頁面為影像"),
                             tr("這個環境沒有可用的影像格式外掛"));
        return;
    }

    bool ok = false;
    const int dpi = QInputDialog::getInt(this, tr("匯出頁面為影像"), tr("解析度（DPI）"), 150, 36,
                                         1200, 6, &ok);
    if (!ok) return;

    QFileDialog dialog(this, tr("匯出頁面為影像"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilters(filters);
    const int pageNumber = pageView_->pageIndex() + 1;
    dialog.selectFile(QFileInfo(currentPath_).completeBaseName() +
                      tr("_第%1頁").arg(pageNumber) + QStringLiteral(".") +
                      QString::fromLatin1(available.front()->suffix));
    if (dialog.exec() != QDialog::Accepted) return;
    const QStringList selected = dialog.selectedFiles();
    if (selected.isEmpty()) return;

    // 格式取自使用者選的篩選器，不是副檔名：使用者可能手打了不相符的副檔名，
    // 而依副檔名靜默改寫格式會讓「我選了 PNG 卻得到 JPEG」。
    const int filterIndex =
        std::max<int>(0, static_cast<int>(filters.indexOf(dialog.selectedNameFilter())));
    const FormatEntry& chosen = *available[static_cast<std::size_t>(filterIndex)];

    QFile source(currentPath_);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("匯出頁面為影像"), tr("無法讀取目前的文件"));
        return;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    engine::iosec::ImageExportOptions options;
    options.dpi = dpi;
    options.format = chosen.format;
    const engine::iosec::ImageExportResult result = engine::iosec::exportPageImage(
        std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
        pageView_->pageIndex(), selected.first().toStdString(), options);

    if (result.ok()) {
        statusBar()->showMessage(tr("已匯出第 %1 頁（%2 KB）")
                                     .arg(pageNumber)
                                     .arg(result.bytesWritten / 1024),
                                 5000);
    } else {
        QMessageBox::warning(this, tr("匯出頁面為影像"),
                             QString::fromUtf8(engine::iosec::describe(result.status)) +
                                 QStringLiteral("\n") + QString::fromStdString(result.message));
    }
}

void MainWindow::exportDocumentText() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("匯出純文字"), tr("尚未開啟文件"));
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("匯出純文字"),
        QFileInfo(currentPath_).completeBaseName() + QStringLiteral(".txt"),
        tr("純文字檔 (*.txt)"));
    if (target.isEmpty()) return;

    // 自己開一份擷取器而不是借用選取控制器的：匯出是一次性的長工作，
    // 借用會讓使用者在匯出期間的選字與搜尋全部排在它後面。
    // PDFium 的行程級序列化仍然成立（ADR-005），只是不搶同一條佇列。
    auto extractor = std::make_shared<engine::text::TextExtractor>();
    auto* window = this;
    extractor->open(currentPath_.toStdString(), "",
                    [extractor, window, target](domain::DocumentError error) {
                        if (error != domain::DocumentError::None) {
                            QMetaObject::invokeMethod(
                                window,
                                [window] {
                                    QMessageBox::warning(window, tr("匯出純文字"),
                                                         tr("無法開啟文件以擷取文字"));
                                },
                                Qt::QueuedConnection);
                            return;
                        }
                        engine::iosec::exportDocumentText(
                            *extractor, target.toStdString(),
                            [extractor, window](engine::iosec::TextExportResult result) {
                                // extractor 以值捕捉，讓它活到回呼結束；**在這裡明確
                                // 釋放不安全**——解構會 join 文字執行緒，而這個回呼
                                // 正在那條執行緒上，自我 join 會 std::terminate。
                                // 交給投遞出去的 lambda 在 GUI 執行緒上釋放。
                                QMetaObject::invokeMethod(
                                    window,
                                    [extractor, window, result] {
                                        if (result.ok()) {
                                            window->statusBar()->showMessage(
                                                tr("已匯出 %1 頁的文字").arg(result.pagesExported),
                                                5000);
                                        } else {
                                            QMessageBox::warning(
                                                window, tr("匯出純文字"),
                                                QString::fromUtf8(
                                                    engine::iosec::describe(result.status)));
                                        }
                                    },
                                    Qt::QueuedConnection);
                            });
                    });
}

void MainWindow::applyViewMode() {
    const domain::ViewMode mode = presentation_.mode();
    const bool presenting = mode == domain::ViewMode::Presentation;
    const bool windowed = mode == domain::ViewMode::Normal;

    // 簡報模式藏掉一切介面。**面板的可見性要記下來再還原**，
    // 否則退出簡報之後使用者精心排好的面板全部不見了，而那不是他要求的。
    if (presenting && panelsHiddenForPresentation_.empty()) {
        for (QDockWidget* dock : findChildren<QDockWidget*>()) {
            if (!dock->isVisible()) continue;
            panelsHiddenForPresentation_.push_back(dock);
            dock->hide();
        }
    } else if (!presenting && !panelsHiddenForPresentation_.empty()) {
        for (QDockWidget* dock : panelsHiddenForPresentation_) {
            if (dock != nullptr) dock->show();
        }
        panelsHiddenForPresentation_.clear();
    }

    if (ribbonHost_ != nullptr) ribbonHost_->setVisible(!presenting && ribbonWasVisible_);
    statusBar()->setVisible(!presenting);

    if (windowed) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void MainWindow::fitVisible() {
    // PRD-ZOOM-003 Fit Visible：忽略白邊，讓實際有內容的範圍填滿視窗。
    //
    // 掃描頁面像素找內容外框，是架構明列的整頁光柵化例外之一（低解析度、一次性）
    // ——與「裁切至白邊」用的是同一個偵測器，不另寫一套判準。兩處若各自實作，
    // 使用者會看到「Fit Visible 的範圍」與「裁切掉的白邊」對不起來。
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;

    QFile file(currentPath_);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = file.readAll();
    file.close();

    engine::pages::PageEditor editor;
    if (!editor.open(currentPath_.toStdString(), "")) {
        statusBar()->showMessage(tr("無法解析文件結構"), 5000);
        return;
    }

    const int page = pageView_->pageIndex();
    const std::optional<domain::RectF> bounds = editor.detectContentBounds(page);
    if (!bounds.has_value() || bounds->isEmpty()) {
        // 整頁皆白：沒有內容可以框。退回符合頁面而不是留在原倍率，
        // 使用者按了這個功能總得有事情發生。
        pageView_->fitPage();
        statusBar()->showMessage(tr("這一頁沒有可見內容，已改用符合頁面"), 4000);
        return;
    }

    const bool swap = domain::swapsAxes(pageView_->rotation());
    const double w = swap ? bounds->height() : bounds->width();
    const double h = swap ? bounds->width() : bounds->height();
    if (w <= 0.0 || h <= 0.0) return;

    const double margin = pageView_->layoutOptions().pageGapPx * 2.0;
    pageView_->setScale(std::min((pageView_->width() - margin) / w,
                                 (pageView_->height() - margin) / h));
    // 縮放之後把內容範圍捲到視窗裡，否則使用者看到的是放大後的白邊。
    const domain::PageTransform transform = pageView_->currentPageTransform();
    const domain::PointF topLeft = transform.toDevice(domain::PointF{bounds->left, bounds->top});
    pageView_->scrollToPointInCurrentPage(topLeft);
    statusBar()->showMessage(tr("已縮放至內容範圍"), 3000);
}

void MainWindow::renameCurrentDocument() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("重新命名"), tr("尚未開啟文件"));
        return;
    }

    const QFileInfo info(currentPath_);
    bool accepted = false;
    const QString newBaseName =
        QInputDialog::getText(this, tr("重新命名文件"), tr("新檔名（不含副檔名）"),
                              QLineEdit::Normal, info.completeBaseName(), &accepted);
    if (!accepted) return;

    const app::TabTitleModel::RenamePlan plan =
        app::TabTitleModel::planRename(currentPath_, newBaseName);
    if (!plan.ok) {
        QMessageBox::warning(this, tr("重新命名"), plan.error);
        return;
    }
    if (plan.newPath == currentPath_) return;

    if (QFileInfo::exists(plan.newPath)) {
        // 不提供「覆蓋」：改名覆蓋掉另一份 PDF 是無法復原的，而使用者想要的
        // 幾乎一定是換個名字，不是刪掉那一份。
        QMessageBox::warning(this, tr("重新命名"),
                             tr("同一個資料夾裡已經有 %1 了，請換一個名稱。")
                                 .arg(QFileInfo(plan.newPath).fileName()));
        return;
    }

    // 檔案正開著。SharedReadFile 以 FILE_SHARE_DELETE 開啟，因此更名不會被擋下——
    // 那正是那個旗標存在的理由（見 platform/shared_file.h）。
    if (!QFile::rename(currentPath_, plan.newPath)) {
        QMessageBox::warning(this, tr("重新命名"), tr("改名失敗，檔案可能被其他程式鎖住。"));
        return;
    }

    // 重新開檔而不是只改標題：所有子系統握的都是舊路徑，之後任何一次存檔
    // 都會寫回舊檔名——那個檔案已經不存在了。
    const QString target = plan.newPath;
    // 舊路徑的頁籤要跟著改名，不是留下一個指向已不存在的檔案的頁籤。
    if (const auto located = tabs_.locate(app::normalizeDocumentPath(currentPath_))) {
        (void)tabs_.closeTab(located->first, located->second);
    }
    openPath(target);
    statusBar()->showMessage(tr("已重新命名為 %1").arg(QFileInfo(target).fileName()), 5000);
}

void MainWindow::editPageLabels() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("頁面標籤"), tr("尚未開啟文件"));
        return;
    }
    if (!confirmNoExternalChange(tr("頁面標籤"))) return;

    QFile file(currentPath_);
    if (!file.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(tr("無法讀取目前的文件"), 5000);
        return;
    }
    const QByteArray raw = file.readAll();
    file.close();
    const std::string bytes(raw.constData(), static_cast<std::size_t>(raw.size()));

    engine::objects::IncrementalAppender appender;
    QString diagnostic;
    std::string sourceDiagnostic;
    const engine::objects::SourceStatus status = appender.open(bytes, &sourceDiagnostic);
    if (status != engine::objects::SourceStatus::Ok) {
        QMessageBox::warning(this, tr("頁面標籤"),
                             status == engine::objects::SourceStatus::Encrypted
                                 ? tr("加密文件尚不支援編輯頁面標籤")
                                 : tr("無法解析文件：%1")
                                       .arg(QString::fromStdString(sourceDiagnostic)));
        return;
    }

    PageLabelsDialog dialog(engine::labels::readPageLabels(appender.source()),
                            controller_->pageCount(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    const engine::labels::PageLabelWriteResult written =
        engine::labels::writePageLabels(appender, dialog.result());
    if (!written.ok) {
        QMessageBox::warning(this, tr("頁面標籤"),
                             tr("寫入失敗：%1").arg(QString::fromStdString(written.diagnostic)));
        return;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        QMessageBox::warning(this, tr("頁面標籤"),
                             tr("增量儲存失敗：%1")
                                 .arg(QString::fromStdString(built.diagnostic)));
        return;
    }

    // 與註解寫入同一道防線：增量儲存的前提是原檔位元組原封不動，
    // 一旦不成立，既有簽章會從「有效、簽章後有變更」變成「無效」。
    if (built.bytes.size() < bytes.size() ||
        std::memcmp(built.bytes.data(), bytes.data(), bytes.size()) != 0) {
        QMessageBox::warning(this, tr("頁面標籤"),
                             tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章"));
        return;
    }

    platform::AtomicFileWriter writer(currentPath_);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        QMessageBox::warning(this, tr("頁面標籤"), tr("寫檔失敗"));
        return;
    }

    fileSnapshot_ = platform::captureSnapshot(currentPath_);
    statusBar()->showMessage(tr("已更新頁面標籤（%1 段，增量 %2 位元組）")
                                 .arg(written.rangeCount)
                                 .arg(built.appendedBytes),
                             5000);
    reloadCurrentDocument();
}

std::size_t MainWindow::annotationAt(int pageIndex, const domain::PointF& pagePoint) const {
    const auto& items = controller_->annotations();
    // 由後往前找：/Annots 的順序就是繪製順序，後面的畫在上面，
    // 使用者點到重疊處時期待選到看得見的那一則。
    for (std::size_t i = items.size(); i > 0; --i) {
        const domain::AnnotationSummary& item = items[i - 1];
        if (item.pageIndex != pageIndex) continue;
        // 只認便利貼。
        //
        // 螢光筆與底線的 /Rect 蓋住的是一整段文字，把它們也算進來的話，
        // 使用者想從一段已標記的文字開始選取時就會被彈出一個視窗打斷——
        // 而這個訊號刻意不吃掉事件，選取還是會照常開始，兩件事同時發生。
        // 其他子型的註釋一樣讀得到，走註解清單雙擊那條路徑。
        if (item.subtype != "Text") continue;
        if (item.rect.contains(pagePoint)) return i - 1;
    }
    return items.size();
}

std::size_t MainWindow::annotationAtForSelection(int pageIndex,
                                                 const domain::PointF& pagePoint) const {
    const auto& items = controller_->annotations();
    // 由後往前：/Annots 的順序就是繪製順序，後面的畫在上面，
    // 使用者點到重疊處時期待選到看得見的那一則。
    for (std::size_t i = items.size(); i > 0; --i) {
        const domain::AnnotationSummary& item = items[i - 1];
        if (item.pageIndex != pageIndex) continue;
        // Popup 不算：它是別則註解的附屬視窗，不是一則可以獨立選取的標記。
        // 選到它的話，畫面上會出現一個框住空白處的強調框。
        if (item.subtype == "Popup") continue;
        if (item.rect.normalized().contains(pagePoint)) return i - 1;
    }
    return items.size();
}

void MainWindow::selectAnnotation(std::size_t index) {
    const auto& items = controller_->annotations();
    if (index >= items.size()) {
        selectedAnnotation_ = static_cast<std::size_t>(-1);
        pageView_->clearHighlightedRect();
        return;
    }

    selectedAnnotation_ = index;
    const domain::AnnotationSummary& item = items[index];
    pageView_->setHighlightedRect(item.pageIndex, item.rect);

    // 清單上同步選起來。找不到是正常的——目前的篩選條件可能把它濾掉了；
    // 那時只留強調框，不去動使用者設好的篩選條件。
    for (std::size_t row = 0; row < annotationOrder_.size(); ++row) {
        if (annotationOrder_[row] == index) {
            annotationList_->setCurrentRow(static_cast<int>(row));
            break;
        }
    }
    navigateToPage(item.pageIndex);
    statusBar()->showMessage(tr("已選取第 %1 頁的%2（%3）")
                                 .arg(item.pageIndex + 1)
                                 .arg(QString::fromStdString(item.subtype))
                                 .arg(item.author.empty() ? tr("未署名")
                                                          : QString::fromStdString(item.author)),
                             4000);
}

void MainWindow::goToAdjacentAnnotation(int direction) {
    const auto& items = controller_->annotations();
    if (items.empty()) {
        statusBar()->showMessage(tr("這份文件沒有註解"), 3000);
        return;
    }

    const std::size_t next =
        domain::adjacentInDocumentOrder(items, selectedAnnotation_, direction);
    if (next >= items.size()) {
        statusBar()->showMessage(direction > 0 ? tr("已經是最後一則註解")
                                               : tr("已經是第一則註解"),
                                 3000);
        return;
    }
    selectAnnotation(next);
}

void MainWindow::openNotePopup(std::size_t index) {
    const auto& items = controller_->annotations();
    if (index >= items.size()) return;
    const domain::AnnotationSummary& item = items[index];

    if (notePopup_ == nullptr) {
        notePopup_ = new StickyNotePopup(this);
        connect(notePopup_, &StickyNotePopup::contentsCommitted, this,
                &MainWindow::commitNoteContents);
        // 視窗關閉時取消清單上的關聯選取，否則清單會停在一個「被選中」的狀態，
        // 而使用者已經看不到那一則對應到哪裡。
        connect(notePopup_, &StickyNotePopup::dismissed, this,
                [this] { annotationList_->setCurrentRow(-1); });
    }

    // 權限不允許加註時仍然可以看，只是不能改（PRD-SEC-002）。
    notePopup_->setReadOnly(!controller_->info().permissions.annotate);
    notePopup_->showAnnotation(item.pageIndex, item.indexOnPage,
                               QString::fromStdString(item.subtype),
                               QString::fromStdString(item.author),
                               QString::fromStdString(item.modified),
                               QString::fromStdString(item.contents));

    // 另一個方向的連結：視窗開了，清單也要跳到同一則。找不到是正常的
    // ——目前的篩選條件可能把它濾掉了——那時只留視窗，不去動篩選條件。
    for (std::size_t row = 0; row < annotationOrder_.size(); ++row) {
        if (annotationOrder_[row] == index) {
            annotationList_->setCurrentRow(static_cast<int>(row));
            break;
        }
    }
    navigateToPage(item.pageIndex);
}

void MainWindow::commitNoteContents(int pageIndex, int indexOnPage, const QString& contents) {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.annotate) {
        statusBar()->showMessage(tr("此文件的權限設定不允許加註"), 5000);
        return;
    }
    if (!confirmNoExternalChange(tr("編輯註釋"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("編輯註釋");
    command.redo = [this, path, pageIndex, indexOnPage, contents, state] {
        *state = annotations_->updateAnnotationContents(path, pageIndex, indexOnPage, contents);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("編輯註釋"),
                             state->message.isEmpty() ? tr("寫入失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::applyPencilStroke(int pageIndex,
                                   const std::vector<domain::PressurePoint>& points) {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.annotate) {
        statusBar()->showMessage(tr("此文件的權限設定不允許加註"), 5000);
        return;
    }
    if (points.size() < 2) return;

    const std::vector<domain::PressurePoint> smoothed = domain::smoothStroke(points);
    const std::vector<domain::InkWidthLayer> layers = domain::splitByPressure({smoothed});
    if (layers.empty()) return;

    // /Rect 是整筆的外接矩形，各層共用。用各層自己的外接矩形會讓細的那一層
    // 只在它自己那一段可以點到，而使用者眼中那是同一條線。
    domain::RectF bounds{points.front().position.x, points.front().position.y,
                         points.front().position.x, points.front().position.y};
    for (const domain::PressurePoint& point : smoothed) {
        bounds.left = std::min(bounds.left, point.position.x);
        bounds.bottom = std::min(bounds.bottom, point.position.y);
        bounds.right = std::max(bounds.right, point.position.x);
        bounds.top = std::max(bounds.top, point.position.y);
    }

    const QString author = annotationAuthor();

    std::vector<app::XfdfEntry> entries;
    entries.reserve(layers.size());
    for (const domain::InkWidthLayer& layer : layers) {
        domain::Annotation annotation;
        annotation.color = domain::ColorRgb{0.85, 0.1, 0.1};
        annotation.border.width = layer.width;
        // 外接矩形往外撐半個線寬：粗線的筆畫會畫到座標之外，/Rect 貼齊座標
        // 會讓 Acrobat 把邊緣裁掉。
        const double pad = layer.width * 0.5;
        annotation.rect = domain::RectF{bounds.left - pad, bounds.bottom - pad,
                                        bounds.right + pad, bounds.top + pad};
        domain::InkGeometry geometry;
        geometry.strokes = layer.strokes;
        annotation.geometry = std::move(geometry);

        app::XfdfEntry entry;
        entry.pageIndex = pageIndex;
        entry.annotation = app::AnnotationService::stamped(std::move(annotation), author);
        entries.push_back(std::move(entry));
    }

    if (!confirmNoExternalChange(tr("鉛筆"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("鉛筆");
    command.redo = [this, path, entries, state] {
        *state = annotations_->addAnnotations(path, entries);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        statusBar()->showMessage(
            state->message.isEmpty() ? tr("鉛筆筆畫寫入失敗") : state->message, 8000);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::flattenAnnotations() {
    if (!controller_->isOpen()) {
        QMessageBox::information(this, tr("攤平註解"), tr("尚未開啟文件"));
        return;
    }

    const QString path = currentPath_;
    const auto summary = annotationFlatten_->preview(path);
    if (summary.flattened == 0) {
        QMessageBox::information(this, tr("攤平註解"), tr("這份文件沒有可攤平的註解"));
        return;
    }

    // 說出會影響幾則。只問「確定要攤平嗎」的話，使用者沒有辦法判斷自己是不是
    // 開錯了檔案。略過的則數也要講——靜靜跳過會讓人以為整份都攤平了。
    QString question = tr("將把 %1 則註解燒進頁面內容，之後它們不再是可編輯的註解。")
                           .arg(summary.flattened);
    if (summary.skipped > 0) {
        question += QStringLiteral("\n") +
                    tr("另有 %1 則無法攤平（表單欄位、圖章等），會原樣留下。")
                        .arg(summary.skipped);
    }
    question += QStringLiteral("\n\n") + tr("要繼續嗎？");
    if (QMessageBox::question(this, tr("攤平註解"), question,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (!confirmNoExternalChange(tr("攤平註解"))) return;

    // 走命令堆疊。攤平在語意上不可逆，但檔案層次上仍是純附加，
    // 所以「攤錯了」還救得回來——復原就是把檔案截回原長度。
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("攤平註解");
    const QString label = command.label;
    command.redo = [this, path, state] {
        *state = annotationFlatten_->flattenAll(path, domain::IrreversibleConsent::confirmed());
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        statusBar()->showMessage(state->message.isEmpty() ? label + tr("失敗") : state->message,
                                 8000);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::exportCommentSummary() {
    if (!controller_->isOpen()) {
        QMessageBox::information(this, tr("摘要註解"), tr("尚未開啟文件"));
        return;
    }
    const auto entries = app::CommentSummaryService::build(controller_->annotations());
    if (entries.empty()) {
        QMessageBox::information(this, tr("摘要註解"), tr("這份文件沒有註解"));
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("摘要註解"),
        QFileInfo(currentPath_).completeBaseName() + QStringLiteral("-comments.txt"),
        tr("純文字檔 (*.txt)"));
    if (target.isEmpty()) return;

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("摘要註解"), tr("無法寫入 %1").arg(target));
        return;
    }
    out.write(app::CommentSummaryService::renderSummaryOnlyText(entries).toUtf8());
    out.close();
    statusBar()->showMessage(tr("已摘要 %1 則註解到 %2").arg(entries.size()).arg(target), 5000);
}

void MainWindow::exportDocumentWithSummary(SummaryLayout layout) {
    const QString title = layout == SummaryLayout::SideBySide ? tr("並排摘要") : tr("文件加摘要");
    const QString suffix = layout == SummaryLayout::SideBySide
                               ? QStringLiteral("-side-by-side.pdf")
                               : QStringLiteral("-summary.pdf");

    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, title, tr("尚未開啟文件"));
        return;
    }
    const auto entries = app::CommentSummaryService::build(controller_->annotations());
    if (entries.empty()) {
        QMessageBox::information(this, title, tr("這份文件沒有註解"));
        return;
    }

    std::vector<std::pair<int, QString>> summaries;
    for (const int page : app::CommentSummaryService::pagesWithComments(entries)) {
        QString text = app::CommentSummaryService::renderPageSummaryText(entries, page);
        if (text.isEmpty()) continue;
        summaries.emplace_back(page, std::move(text));
    }
    if (summaries.empty()) {
        QMessageBox::information(this, title, tr("這份文件沒有註解"));
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, title, QFileInfo(currentPath_).completeBaseName() + suffix,
        tr("PDF 檔案 (*.pdf)"));
    if (target.isEmpty()) return;
    if (QFileInfo(target) == QFileInfo(currentPath_)) {
        // 產出的是一份新文件，不是對原檔的編輯。覆蓋原檔會讓使用者失去
        // 沒有摘要頁的版本，而那正是他之後還要繼續審閱的那一份。
        QMessageBox::warning(this, title,
                             tr("請選擇與原文件不同的檔名——摘要頁會寫進新檔，不動原檔。"));
        return;
    }

    if (!QFile::copy(currentPath_, target)) {
        // getSaveFileName 已經問過覆蓋，所以這裡先移除舊檔再複製。
        if (!QFile::remove(target) || !QFile::copy(currentPath_, target)) {
            QMessageBox::warning(this, title, tr("無法建立 %1").arg(target));
            return;
        }
    }

    // 畫不出來的字（內嵌子集裡沒有的字形）會讓排版明確失敗而不是靜默丟字。
    // 失敗時把剛複製的檔案清掉，不留下一份沒加摘要卻叫做 -summary.pdf 的檔案。
    const app::PageOperationResult result =
        layout == SummaryLayout::SideBySide
            ? pageOps_->insertSideBySideSummary(target, summaries,
                                                app::RewriteConsent::confirmed())
            : pageOps_->insertSummaryPages(target, summaries,
                                           app::RewriteConsent::confirmed());
    if (!result.ok) {
        QFile::remove(target);
        QMessageBox::warning(this, title, result.message);
        return;
    }
    statusBar()->showMessage(tr("%1（%2）").arg(result.message, target), 8000);
}

void MainWindow::exportAnnotationsToFile(ExportScope scope) {
    const QString title =
        scope == ExportScope::SelectedOnly ? tr("匯出選定註解") : tr("匯出註解");

    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, title, tr("尚未開啟文件"));
        return;
    }
    if (!controller_->info().permissions.copy) {
        QMessageBox::warning(this, title, tr("此文件的權限設定不允許複製內容"));
        return;
    }

    // 選定範圍先問：使用者什麼都沒選就跳出存檔對話框，選完檔名才被告知
    // 「沒有選定任何註解」，那個順序是反的。
    std::vector<domain::AnnotationSummary> chosen;
    if (scope == ExportScope::SelectedOnly) {
        const auto& items = controller_->annotations();
        for (const QModelIndex& index : annotationList_->selectionModel()->selectedRows()) {
            const int row = index.row();
            if (row < 0 || row >= static_cast<int>(annotationOrder_.size())) continue;
            const std::size_t original = annotationOrder_[static_cast<std::size_t>(row)];
            if (original < items.size()) chosen.push_back(items[original]);
        }
        if (chosen.empty()) {
            QMessageBox::information(this, title,
                                     tr("請先在註解清單裡選取要匯出的註解"));
            return;
        }
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("匯出註解"), QFileInfo(currentPath_).completeBaseName() + QStringLiteral(".xfdf"),
        tr("XFDF 註解 (*.xfdf);;FDF 註解 (*.fdf)"));
    if (target.isEmpty()) return;

    QString error;
    std::vector<app::XfdfEntry> entries =
        annotations_->readAnnotationsForExport(currentPath_, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, title, error);
        return;
    }
    if (entries.empty()) {
        QMessageBox::information(this, title, tr("這份文件沒有可匯出的註解"));
        return;
    }

    int requested = 0;
    if (scope == ExportScope::SelectedOnly) {
        requested = static_cast<int>(chosen.size());
        entries = app::AnnotationService::selectEntries(entries, chosen);
        if (entries.empty()) {
            // 選了東西卻一則都對不上，代表檔案在這期間被別的程式改過。
            // 寫出一份空的 XFDF 是最糟的結果：它看起來成功了。
            QMessageBox::warning(this, title,
                                 tr("選取的註解與檔案內容對不上，可能已被其他程式修改。"
                                    "請重新開啟文件後再試。"));
            return;
        }
    }

    const std::string sourceName = QFileInfo(currentPath_).fileName().toStdString();
    const bool asFdf = target.endsWith(QStringLiteral(".fdf"), Qt::CaseInsensitive);
    const std::string bytes = asFdf ? app::exportFdf(entries, sourceName)
                                    : app::exportXfdf(entries, sourceName);

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, title, tr("無法寫入 %1").arg(target));
        return;
    }
    out.write(bytes.data(), static_cast<qint64>(bytes.size()));
    out.close();

    // 選了 5 則卻只匯出 4 則一定要說出來。差額的來源是比對不上的項目，
    // 而使用者拿到的檔案看起來完全正常。
    if (scope == ExportScope::SelectedOnly && static_cast<int>(entries.size()) < requested) {
        QMessageBox::warning(this, title,
                             tr("選取了 %1 則，只有 %2 則對得上檔案內容並已匯出。")
                                 .arg(requested)
                                 .arg(entries.size()));
    }
    statusBar()->showMessage(tr("已匯出 %1 則註解到 %2").arg(entries.size()).arg(target), 5000);
}

void MainWindow::importAnnotationsFromFile() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("匯入註解"), tr("尚未開啟文件"));
        return;
    }
    if (!controller_->info().permissions.annotate) {
        QMessageBox::warning(this, tr("匯入註解"), tr("此文件的權限設定不允許加註"));
        return;
    }

    const QString source = QFileDialog::getOpenFileName(
        this, tr("匯入註解"), QString(),
        tr("註解檔 (*.xfdf *.fdf);;XFDF 註解 (*.xfdf);;FDF 註解 (*.fdf)"));
    if (source.isEmpty()) return;

    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("匯入註解"), tr("無法讀取檔案：%1").arg(in.errorString()));
        return;
    }
    const QByteArray raw = in.readAll();
    in.close();
    const std::string bytes(raw.constData(), static_cast<std::size_t>(raw.size()));

    std::vector<app::XfdfEntry> entries;
    QStringList skipped;
    // 格式由內容而不是副檔名判定：使用者收到的檔案常常副檔名與內容不符
    // （信箱另存時被改名、對方存錯），而兩種格式的開頭是明確可分的。
    if (bytes.rfind("%FDF-", 0) == 0) {
        const app::FdfImportResult result = app::importFdf(bytes);
        if (!result.ok) {
            QMessageBox::warning(this, tr("匯入註解"),
                                 QString::fromStdString(result.diagnostic));
            return;
        }
        entries = result.entries;
        for (const std::string& item : result.skipped) skipped << QString::fromStdString(item);
    } else {
        const app::XfdfImportResult result = app::importXfdf(bytes);
        if (!result.ok) {
            QMessageBox::warning(this, tr("匯入註解"),
                                 QString::fromStdString(result.diagnostic));
            return;
        }
        entries = result.entries;
        for (const std::string& item : result.skippedElements) {
            skipped << QString::fromStdString(item);
        }
    }

    if (!confirmNoExternalChange(tr("匯入註解"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("匯入註解");
    command.redo = [this, path, entries, state] {
        *state = annotations_->addAnnotations(path, entries);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("匯入註解"),
                             state->message.isEmpty() ? tr("匯入失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);

    QString message = state->message;
    if (!skipped.isEmpty()) {
        // 跳過的項目一定要說出來。少收了幾則註解而畫面上沒有任何提示，
        // 使用者要到對方問「我的意見呢」才會發現。
        message += tr("；來源檔中有 %1 項未支援：%2")
                       .arg(skipped.size())
                       .arg(skipped.mid(0, 5).join(QStringLiteral("、")));
    }
    statusBar()->showMessage(message, 8000);
}

void MainWindow::markRedaction(int pageIndex, const domain::RectF& pageRect) {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.modify) {
        statusBar()->showMessage(tr("此文件的權限設定不允許修改"), 5000);
        return;
    }
    if (!confirmNoExternalChange(tr("標記待塗黑"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("標記待塗黑");
    command.redo = [this, path, pageIndex, pageRect, state] {
        *state = redaction_->markArea(path, pageIndex, pageRect);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        statusBar()->showMessage(state->message.isEmpty() ? tr("標記失敗") : state->message, 8000);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::signCurrentDocument() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("數位簽署"), tr("尚未開啟文件"));
        return;
    }
    // 不需要檢查「有沒有未存檔的修改」：本產品的每一個文件修改都是當下就
    // 增量寫入磁碟（命令堆疊的復原是把檔案截回原長度），磁碟上的內容永遠
    // 就是眼前這一份。真正要擋的是**別的程式**在這期間動過檔案。
    if (!confirmNoExternalChange(tr("數位簽署"))) return;

    SignDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    const app::SignatureController::SigningOutcome outcome =
        signatures_->signDocument(currentPath_, dialog.request());
    if (!outcome.ok) {
        QMessageBox::warning(this, tr("數位簽署"), outcome.message);
        return;
    }

    // 簽署刻意**不進命令堆疊**。
    //
    // 「復原簽署」聽起來合理，但它的實作是把檔案截回原長度——那和刪掉簽章
    // 是同一件事，而使用者對 Ctrl+Z 的預期不包含「把一份已簽署的文件變回
    // 未簽署」。要移除簽章應該是一個講清楚後果的獨立動作。
    commands_->clear();
    updateUndoActions();
    fileSnapshot_ = platform::captureSnapshot(currentPath_);
    reloadCurrentDocument();
    signatures_->verify();
    statusBar()->showMessage(outcome.message, 8000);
}

void MainWindow::saveDocumentAs() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("另存新檔"), tr("尚未開啟文件"));
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("另存新檔"),
        QFileInfo(currentPath_).completeBaseName() + QStringLiteral("-copy.pdf"),
        tr("PDF 檔案 (*.pdf)"));
    if (target.isEmpty()) return;
    if (QFileInfo(target) == QFileInfo(currentPath_)) {
        QMessageBox::information(this, tr("另存新檔"), tr("這就是目前的檔案。"));
        return;
    }

    // getSaveFileName 已經問過覆蓋，所以這裡先移除再複製——QFile::copy
    // 不會覆蓋既有檔案，少了這一步「覆蓋」會靜靜地失敗。
    if (QFile::exists(target) && !QFile::remove(target)) {
        QMessageBox::warning(this, tr("另存新檔"), tr("無法覆蓋 %1").arg(target));
        return;
    }
    if (!QFile::copy(currentPath_, target)) {
        QMessageBox::warning(this, tr("另存新檔"), tr("無法寫入 %1").arg(target));
        return;
    }

    // 切換到新檔繼續編輯（與 Acrobat 一致）。但要說清楚原檔的狀態：
    // 這個架構下原檔的修改早就落盤了，「另存新檔」是分岔出一份副本，
    // 不是「把未存檔的東西存到別處」——不講的話使用者會以為原檔回到了
    // 乾淨的狀態（ADR-008）。
    openPath(target);
    statusBar()->showMessage(
        tr("已另存為 %1；原檔 %2 保留先前所有修改")
            .arg(QFileInfo(target).fileName(), QFileInfo(currentPath_).fileName()),
        8000);
}

void MainWindow::revertAllChanges() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!commands_->canUndo()) {
        statusBar()->showMessage(tr("本次開啟後沒有做過任何修改"), 4000);
        return;
    }

    // 講明只回得到「本次開啟時」而不是「上次儲存時」——這個架構下沒有
    // 上次儲存那個時間點（ADR-008）。不講的話使用者會以為能回到昨天。
    const auto answer = QMessageBox::question(
        this, tr("復原所有修改"),
        tr("將復原本次開啟這份文件之後做的所有修改。\n\n"
           "回不到更早的版本——上一次關閉之前的修改已經寫進檔案了。要繼續嗎？"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    int undone = 0;
    while (commands_->canUndo()) {
        if (!commands_->undo()) {
            // 一步復原不了就停下來並說出來，不繼續往下試。剩下的步驟多半
            // 也回不去（例如檔案被別的程式改過），硬做只會讓狀態更亂。
            statusBar()->showMessage(
                tr("已復原 %1 步，其餘步驟無法復原（檔案可能已被其他程式修改）").arg(undone),
                8000);
            updateUndoActions();
            return;
        }
        ++undone;
    }
    updateUndoActions();
    statusBar()->showMessage(tr("已復原 %1 步修改").arg(undone), 5000);
}

void MainWindow::applyToolPersistence() {
    // PRD-UI-007：用完一個註解工具之後，下一個作用中的工具該是什麼。
    // 規則本身是 ToolPersistenceModel 的一行純函數，這裡只負責把結果
    // 套回工具列——包括讓對應的 QAction 打勾，否則工具切回選取了，
    // 工具列上仍然反白著剛才那顆按鈕。
    if (toolPersistence_.stickyEnabled()) return;
    if (pageView_->tool() == Tool::Select) return;

    pageView_->setTool(Tool::Select);
    if (QAction* select = actionRegistry_->action(QStringLiteral("tool.select"));
        select != nullptr) {
        select->setChecked(true);
    }
}

void MainWindow::compareWithDocument() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("比較文件"), tr("尚未開啟文件"));
        return;
    }

    const QString other = QFileDialog::getOpenFileName(
        this, tr("選擇要比較的 PDF"), QFileInfo(currentPath_).absolutePath(),
        tr("PDF 檔案 (*.pdf)"));
    if (other.isEmpty()) return;
    if (QFileInfo(other) == QFileInfo(currentPath_)) {
        QMessageBox::information(this, tr("比較文件"), tr("兩邊是同一個檔案。"));
        return;
    }

    // 比對要把兩份文件的文字全部擷取出來，500 頁的文件會跑上幾秒。
    // 沒有游標提示的話，使用者會以為程式當掉並開始亂點。
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const engine::compare::CompareResult result =
        engine::compare::compareDocuments(other.toStdString(), currentPath_.toStdString());
    QApplication::restoreOverrideCursor();

    if (!result.ok()) {
        // 指名是哪一份出錯：兩份都可能是壞的，而使用者只選了其中一份。
        const QString which = result.oldDocumentFailed ? other : currentPath_;
        QMessageBox::warning(this, tr("比較文件"),
                             tr("無法讀取 %1").arg(QFileInfo(which).fileName()));
        return;
    }

    // 對話框持有 diff 的參照，所以 diff 必須活得比它久。用一個隨對話框
    // 一起銷毀的複本，而不是把 result 留在堆疊上等對話框 exec 完——
    // 那在非模態的情況下就是懸空參照。
    auto* diff = new domain::DocumentDiff(result.diff);
    auto* dialog =
        new CompareDialog(*diff, QFileInfo(other).fileName(),
                          QFileInfo(currentPath_).fileName(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [diff] { delete diff; });
    connect(dialog, &CompareDialog::pageActivated, this, [this](int newPage) {
        if (newPage >= 0) navigateToPage(newPage);
    });
    dialog->show();
}

std::vector<int> MainWindow::parsePageRange(const QString& text) const {
    // 規則本身住在 domain::parsePageRange（純函數、有邊界測試）。這裡只負責
    // 把 QString 換成 std::string_view 並帶上目前的總頁數。
    const std::string utf8 = text.toStdString();
    return domain::parsePageRange(utf8, controller_->pageCount());
}

void MainWindow::mergeDocuments() {
    // 目前開著的文件自動成為第一份，其餘由使用者挑。順序即頁面順序，
    // 所以用 getOpenFileNames 的回傳順序而不是重新排序——使用者在對話框裡
    // 的選取順序就是他要的順序。
    const QStringList extra = QFileDialog::getOpenFileNames(
        this, tr("選擇要合併的 PDF（依選取順序接在目前文件之後）"),
        currentPath_.isEmpty() ? QString() : QFileInfo(currentPath_).absolutePath(),
        tr("PDF 檔案 (*.pdf)"));
    if (extra.isEmpty()) return;

    QStringList sources;
    if (!currentPath_.isEmpty() && controller_->isOpen()) sources << currentPath_;
    sources << extra;

    const QString target = QFileDialog::getSaveFileName(
        this, tr("合併後另存"), QStringLiteral("merged.pdf"), tr("PDF 檔案 (*.pdf)"));
    if (target.isEmpty()) return;

    const app::PageOperationResult result = pageOps_->mergeDocuments(sources, target);
    if (!result.ok) {
        QMessageBox::warning(this, tr("合併文件"), result.message);
        return;
    }
    // 不進命令堆疊：沒有任何來源被改過，沒有東西要復原。
    statusBar()->showMessage(result.message, 8000);
}

void MainWindow::resizePagesWithDialog() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("調整頁面尺寸"), tr("尚未開啟文件"));
        return;
    }

    // 紙張清單刻意很短。要的是「換成常見紙張」這件事，而任意尺寸輸入框
    // 會讓對話框多兩個欄位、多兩種錯誤輸入，卻服務不到多少人。
    struct PaperChoice {
        const char* label;
        double widthPt;
        double heightPt;
    };
    static const PaperChoice kChoices[] = {
        {"A4（直向）", 595.276, 841.89},    {"A4（橫向）", 841.89, 595.276},
        {"A3（直向）", 841.89, 1190.55},    {"A3（橫向）", 1190.55, 841.89},
        {"Letter（直向）", 612.0, 792.0},   {"Letter（橫向）", 792.0, 612.0},
    };

    QStringList papers;
    for (const PaperChoice& choice : kChoices) papers << tr(choice.label);

    bool accepted = false;
    const QString paper = QInputDialog::getItem(this, tr("調整頁面尺寸"), tr("目標紙張："),
                                                papers, 0, false, &accepted);
    if (!accepted || paper.isEmpty()) return;
    const int paperIndex = papers.indexOf(paper);
    if (paperIndex < 0) return;

    // 縮放政策必須問，不能給預設值：兩種意思差別很大，而且從「調整頁面尺寸」
    // 這個操作本身推斷不出來使用者要哪一種。工程圖選錯就是比例尺被改掉，
    // 而那份圖之後還會被拿去量。
    const QStringList policies{tr("內容跟著縮放（報告、文件）"),
                               tr("內容維持原尺寸並置中（工程圖：比例尺不可變）")};
    const QString policy = QInputDialog::getItem(this, tr("調整頁面尺寸"), tr("內容處理方式："),
                                                 policies, 0, false, &accepted);
    if (!accepted || policy.isEmpty()) return;
    const bool keepContent = policies.indexOf(policy) == 1;

    if (!confirmRewrite(tr("調整頁面尺寸"))) return;

    const domain::SizeF target{kChoices[paperIndex].widthPt, kChoices[paperIndex].heightPt};
    const auto chosen = keepContent ? engine::pageops::ResizePolicy::KeepContent
                                    : engine::pageops::ResizePolicy::ScaleContent;
    commitPageOperation(tr("調整頁面尺寸"), [this, target, chosen] {
        return pageOps_->resizePages(currentPath_, {}, target, chosen,
                                     app::RewriteConsent::confirmed());
    });
}

void MainWindow::splitDocument() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("分割文件"), tr("尚未開啟文件"));
        return;
    }

    bool accepted = false;
    const int perChunk = QInputDialog::getInt(
        this, tr("分割文件"), tr("每份幾頁？（共 %1 頁）").arg(controller_->pageCount()), 1, 1,
        std::max(1, controller_->pageCount()), 1, &accepted);
    if (!accepted) return;

    const QString target = QFileDialog::getSaveFileName(
        this, tr("分割後的檔名樣板"),
        QFileInfo(currentPath_).completeBaseName() + QStringLiteral("-{n}.pdf"),
        tr("PDF 檔案 (*.pdf)"));
    if (target.isEmpty()) return;

    const app::PageOperationResult result =
        pageOps_->splitDocument(currentPath_, perChunk, target);
    if (!result.ok) {
        QMessageBox::warning(this, tr("分割文件"), result.message);
        return;
    }
    statusBar()->showMessage(result.message, 8000);
}

void MainWindow::deletePagesByRange() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.assemble) {
        QMessageBox::warning(this, tr("刪除頁面"), tr("此文件的權限設定不允許重組頁面"));
        return;
    }

    bool accepted = false;
    const QString text = QInputDialog::getText(
        this, tr("刪除頁面"),
        tr("要刪除哪幾頁？（例如 1,3,5-8，共 %1 頁）").arg(controller_->pageCount()),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted) return;

    const std::vector<int> pages = parsePageRange(text);
    if (pages.empty()) {
        QMessageBox::warning(this, tr("刪除頁面"), tr("頁碼範圍不合法或超出文件頁數"));
        return;
    }
    if (!confirmRewrite(tr("刪除頁面"))) return;

    commitPageOperation(tr("刪除頁面"), [this, pages] {
        return pageOps_->deletePages(currentPath_, pages, app::RewriteConsent::confirmed());
    });
}

void MainWindow::duplicatePagesByRange() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.assemble) {
        QMessageBox::warning(this, tr("複製頁面"), tr("此文件的權限設定不允許重組頁面"));
        return;
    }

    bool accepted = false;
    const QString text = QInputDialog::getText(
        this, tr("複製頁面"),
        tr("要複製哪幾頁？（例如 1,3,5-8，共 %1 頁）").arg(controller_->pageCount()),
        QLineEdit::Normal, QString::number(pageView_->pageIndex() + 1), &accepted);
    if (!accepted) return;

    const std::vector<int> pages = parsePageRange(text);
    if (pages.empty()) {
        QMessageBox::warning(this, tr("複製頁面"), tr("頁碼範圍不合法或超出文件頁數"));
        return;
    }
    if (!confirmRewrite(tr("複製頁面"))) return;

    commitPageOperation(tr("複製頁面"), [this, pages] {
        // -1：插在最後一個來源頁的正後方。
        return pageOps_->duplicatePages(currentPath_, pages, -1,
                                        app::RewriteConsent::confirmed());
    });
}

void MainWindow::extractPagesByRange() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.assemble) {
        QMessageBox::warning(this, tr("擷取頁面"), tr("此文件的權限設定不允許重組頁面"));
        return;
    }

    bool accepted = false;
    const QString text = QInputDialog::getText(
        this, tr("擷取頁面"),
        tr("要擷取哪幾頁？（例如 1,3,5-8，共 %1 頁）").arg(controller_->pageCount()),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted) return;

    const std::vector<int> pages = parsePageRange(text);
    if (pages.empty()) {
        QMessageBox::warning(this, tr("擷取頁面"), tr("頁碼範圍不合法或超出文件頁數"));
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("擷取頁面另存"),
        QFileInfo(currentPath_).completeBaseName() + QStringLiteral("-extract.pdf"),
        tr("PDF 檔案 (*.pdf)"));
    if (target.isEmpty()) return;

    // 擷取不動原檔，因此**不進命令堆疊**：沒有東西要復原，而讓它佔一步
    // 會讓使用者按 Ctrl+Z 時以為能取消那個已經寫出去的檔案。
    const app::PageOperationResult result =
        pageOps_->extractPages(currentPath_, pages, target);
    if (!result.ok) {
        QMessageBox::warning(this, tr("擷取頁面"), result.message);
        return;
    }
    statusBar()->showMessage(result.message, 8000);
}

void MainWindow::attachFileAt(int pageIndex, const domain::RectF& pageRect) {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.annotate) {
        statusBar()->showMessage(tr("此文件的權限設定不允許加註"), 5000);
        return;
    }

    // 先框位置再問檔案，而不是反過來：使用者拖出矩形的當下就已經決定了
    // 「放這裡」，先跳檔案對話框會讓那個手勢的結果懸在半空。
    const QString source = QFileDialog::getOpenFileName(this, tr("選擇要附加的檔案"));
    if (source.isEmpty()) return;
    if (QFileInfo(source) == QFileInfo(currentPath_)) {
        // 把文件自己附進自己會讓檔案大小翻倍，而且附件內容是「附加之前」
        // 的那一份——那幾乎不會是使用者要的。
        QMessageBox::information(this, tr("附加檔案"), tr("不能把這份文件附加到它自己裡面。"));
        return;
    }
    if (!confirmNoExternalChange(tr("附加檔案"))) return;

    app::AttachmentRequest request;
    request.path = currentPath_;
    request.sourceFile = source;
    request.author = annotationAuthor();
    request.pageIndex = pageIndex;
    request.rectPt = pageRect;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("附加檔案");
    command.redo = [this, request, state] {
        *state = attachmentService_->addAttachment(request);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("附加檔案"),
                             state->message.isEmpty() ? tr("加入附件失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    applyToolPersistence();
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::exportFormData() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!forms_->hasForm()) {
        QMessageBox::information(this, tr("匯出表單資料"), tr("這份文件沒有表單欄位"));
        return;
    }

    const QString target = QFileDialog::getSaveFileName(
        this, tr("匯出表單資料"),
        QFileInfo(currentPath_).completeBaseName() + QStringLiteral(".fdf"),
        tr("FDF 表單資料 (*.fdf);;XFDF 表單資料 (*.xfdf)"));
    if (target.isEmpty()) return;

    const engine::forms::FormDataFormat format =
        target.endsWith(QStringLiteral(".xfdf"), Qt::CaseInsensitive)
            ? engine::forms::FormDataFormat::Xfdf
            : engine::forms::FormDataFormat::Fdf;

    // 回呼在表單佇列的執行緒上，碰 widget 之前一定要排回 GUI 執行緒。
    forms_->exportData(format, [this, target](QString text) {
        QMetaObject::invokeMethod(
            this,
            [this, target, text] {
                if (text.isEmpty()) {
                    QMessageBox::warning(this, tr("匯出表單資料"), tr("沒有可匯出的欄位值"));
                    return;
                }
                QFile file(target);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QMessageBox::warning(this, tr("匯出表單資料"),
                                         tr("無法寫入 %1").arg(target));
                    return;
                }
                file.write(text.toUtf8());
                file.close();
                statusBar()->showMessage(tr("已匯出表單資料到 %1").arg(target), 6000);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::importFormData() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.fillForms) {
        QMessageBox::warning(this, tr("匯入表單資料"), tr("此文件的權限設定不允許填寫表單"));
        return;
    }

    const QString source = QFileDialog::getOpenFileName(
        this, tr("匯入表單資料"), QString(),
        tr("表單資料 (*.fdf *.xfdf);;FDF (*.fdf);;XFDF (*.xfdf)"));
    if (source.isEmpty()) return;

    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("匯入表單資料"),
                             tr("無法讀取檔案：%1").arg(file.errorString()));
        return;
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();

    // 格式由內容判定而不是副檔名：使用者收到的檔案常常名實不符
    // （信箱另存時被改名、對方存錯）。
    const std::optional<engine::forms::FormDataFormat> detected =
        engine::forms::detectFormat(text.toStdString());
    if (!detected.has_value()) {
        QMessageBox::warning(this, tr("匯入表單資料"),
                             tr("認不出這個檔案的格式（需要 FDF 或 XFDF）"));
        return;
    }

    forms_->importData(text, *detected, [this](engine::forms::FormDataImport result) {
        QMetaObject::invokeMethod(
            this,
            [this, result] {
                if (!result.ok) {
                    QMessageBox::warning(this, tr("匯入表單資料"),
                                         QString::fromStdString(result.error));
                    return;
                }
                controller_->setDocumentDirty();
                statusBar()->showMessage(
                    tr("已匯入 %1 個欄位值").arg(result.entries.size()), 6000);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::resetFormFields() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.fillForms) {
        QMessageBox::warning(this, tr("重設表單"), tr("此文件的權限設定不允許填寫表單"));
        return;
    }

    // 重設把所有欄位打回預設值，而且不進命令堆疊（表單填寫走 PDFium 的
    // 表單環境，不是物件層通道，沒有截回原長度那條復原路徑）。
    // 所以先問。
    const auto answer = QMessageBox::question(
        this, tr("重設表單"),
        tr("將把所有表單欄位打回預設值。這個動作無法以「復原」還原，要繼續嗎？"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    forms_->resetForm([this](bool ok, QString message) {
        QMetaObject::invokeMethod(
            this,
            [this, ok, message] {
                if (!ok) {
                    QMessageBox::warning(this, tr("重設表單"), message);
                    return;
                }
                controller_->setDocumentDirty();
                statusBar()->showMessage(tr("表單已重設"), 5000);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::createFormField(int pageIndex, const domain::RectF& pageRect) {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.modify) {
        statusBar()->showMessage(tr("此文件的權限設定不允許修改"), 5000);
        return;
    }

    app::FormFieldRequest request;
    request.path = currentPath_;
    request.type = pendingFieldType_;
    request.pageIndex = pageIndex;
    request.rectPt = pageRect;

    // 下拉與清單方塊沒有選項就是一個空的框：看得到、點得開、什麼都沒有。
    // 先問選項再建立，而不是建一個空的讓使用者事後去找哪裡可以加。
    if (request.type == engine::formbuild::BuildFieldType::ComboBox ||
        request.type == engine::formbuild::BuildFieldType::ListBox) {
        bool accepted = false;
        const QString text = QInputDialog::getMultiLineText(
            this, tr("選項"), tr("每行一個選項："), QString(), &accepted);
        if (!accepted) return;
        for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            request.options << line.trimmed();
        }
        if (request.options.isEmpty()) {
            statusBar()->showMessage(tr("沒有輸入任何選項，未建立欄位"), 5000);
            return;
        }
    }

    if (!confirmNoExternalChange(tr("建立表單欄位"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("建立表單欄位");
    command.redo = [this, request, state] {
        *state = formBuild_->addField(request);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("建立表單欄位"),
                             state->message.isEmpty() ? tr("建立失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::applyStamp(StampDialog::Preset preset, const QString& label) {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, label, tr("尚未開啟文件"));
        return;
    }
    if (!controller_->info().permissions.modify) {
        QMessageBox::warning(this, label, tr("此文件的權限設定不允許修改"));
        return;
    }

    StampDialog dialog(preset, this);
    if (dialog.exec() != QDialog::Accepted) return;

    if (!dialog.pageRangeIsValid(controller_->pageCount())) {
        // 不合法時擋下來並說明，而不是靜靜當成「全部頁面」——那會在整份
        // 文件上蓋滿戳記，而使用者輸入範圍正是為了避免這件事。
        QMessageBox::warning(this, label, tr("頁面範圍不合法或超出文件頁數"));
        return;
    }
    app::StampRequest request = dialog.request(controller_->pageCount());
    request.path = currentPath_;

    if (!confirmNoExternalChange(label)) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::StampResult>();
    app::Command command;
    command.label = label;
    command.redo = [this, request, state] {
        *state = stamps_->applyStamps(request);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        // 戳記是純附加（見 stamp_service.h），所以復原就是截回原長度——
        // 與註解走同一條復原路徑，不需要保留整份原始位元組。
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, label,
                             state->message.isEmpty() ? tr("寫入失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 6000);
}

void MainWindow::sanitizeDocument() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("清除隱藏資訊"), tr("尚未開啟文件"));
        return;
    }

    // 說清楚會刪掉什麼再問。「要清除隱藏資訊嗎」不足以讓使用者判斷後果——
    // 嵌入檔案是文件的一部分，刪掉之後收件人拿不到附件，而他不會知道
    // 那是被這一步刪掉的。
    const auto answer = QMessageBox::question(
        this, tr("清除隱藏資訊"),
        tr("將移除下列不會顯示在畫面上、但會跟著檔案散布的資料：\n\n"
           "· 文件資訊（作者、標題、製作程式）\n"
           "· XMP 中繼資料\n"
           "· 製作工具的私有資料（/PieceInfo，常含原始檔路徑）\n"
           "· 嵌入檔案與附件\n"
           "· 內嵌 JavaScript\n\n"
           "這會重寫整份檔案，既有的數位簽章將失效。要繼續嗎？"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    if (!confirmNoExternalChange(tr("清除隱藏資訊"))) return;

    commitPageOperation(tr("清除隱藏資訊"), [this] {
        return redaction_->sanitize(currentPath_, app::RewriteConsent::confirmed());
    });
}

void MainWindow::clearRedactionMarks() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (redaction_->pendingMarkCount(currentPath_) == 0) {
        statusBar()->showMessage(tr("這份文件沒有塗黑標記"), 4000);
        return;
    }
    if (!confirmNoExternalChange(tr("移除塗黑標記"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("移除塗黑標記");
    command.redo = [this, path, state] {
        *state = redaction_->clearMarks(path, -1);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) reloadCurrentDocument();
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        statusBar()->showMessage(state->message, 8000);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::applyRedactionMarks() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.modify) {
        QMessageBox::warning(this, tr("套用塗黑"), tr("此文件的權限設定不允許修改"));
        return;
    }

    const int marks = redaction_->pendingMarkCount(currentPath_);
    if (marks == 0) {
        QMessageBox::information(this, tr("套用塗黑"),
                                 tr("這份文件沒有塗黑標記。請先用「標記待塗黑」框出範圍。"));
        return;
    }

    // 說出會處理幾塊。只問「確定要塗黑嗎」的話，使用者沒有辦法判斷自己
    // 標對了沒有——而這一步之後就沒有機會再確認了。
    const auto answer = QMessageBox::warning(
        this, tr("套用塗黑"),
        tr("即將永久移除 %1 塊標記範圍內的文字、影像與註解。\n\n"
           "這個操作**不可復原**，而且會讓文件裡既有的數位簽章失效。\n"
           "建議先另存一份副本再繼續。\n\n要繼續嗎？")
            .arg(marks),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    if (!confirmNoExternalChange(tr("套用塗黑"))) return;

    commitPageOperation(tr("套用塗黑"), [this] {
        return redaction_->applyMarks(currentPath_, app::RewriteConsent::confirmed());
    });
}

void MainWindow::replyToSelectedAnnotation(const QString& state) {
    const int row = annotationList_->currentRow();
    if (row < 0 || row >= static_cast<int>(annotationOrder_.size())) {
        statusBar()->showMessage(tr("請先在註解清單選一則註解"), 4000);
        return;
    }
    if (!controller_->info().permissions.annotate) {
        statusBar()->showMessage(tr("此文件的權限設定不允許加註"), 5000);
        return;
    }

    const auto& items = controller_->annotations();
    const std::size_t index = annotationOrder_[static_cast<std::size_t>(row)];
    if (index >= items.size()) return;
    const domain::AnnotationSummary target = items[index];

    QString contents;
    if (state.isEmpty()) {
        bool accepted = false;
        contents = QInputDialog::getMultiLineText(this, tr("回覆註解"), tr("回覆內容："),
                                                  QString(), &accepted);
        if (!accepted || contents.trimmed().isEmpty()) return;
    } else {
        // 狀態也可以附一句話，但不強迫——多數時候使用者只是要按「已完成」。
        bool accepted = false;
        contents = QInputDialog::getText(this, tr("設定狀態"), tr("附註（可留空）："),
                                         QLineEdit::Normal, QString(), &accepted);
        if (!accepted) return;
    }

    if (!confirmNoExternalChange(state.isEmpty() ? tr("回覆註解") : tr("設定狀態"))) return;

    const QString path = currentPath_;
    const QString author = annotationAuthor();
    auto shared = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = state.isEmpty() ? tr("回覆註解") : tr("設定狀態");
    command.redo = [this, path, target, contents, author, state, shared] {
        *shared = annotations_->replyToAnnotation(path, target.pageIndex, target.indexOnPage,
                                                  contents, author, state);
        if (shared->ok) reloadCurrentDocument();
        return shared->ok;
    };
    command.undo = [this, path, shared] {
        QString message;
        const bool ok = annotations_->revertAppend(path, shared->previousSize,
                                                   shared->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    // 標題要在 push 之前取出來：push 會把 command 搬走，之後讀它的欄位
    // 是 use-after-move，實務上的症狀是對話框標題變成空的。
    const QString label = command.label;
    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, label,
                             shared->message.isEmpty() ? tr("寫入失敗") : shared->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(shared->message, 5000);
}

void MainWindow::copySelectedAnnotation() {
    const int row = annotationList_->currentRow();
    if (row < 0 || row >= static_cast<int>(annotationOrder_.size())) {
        statusBar()->showMessage(tr("請先在註解清單選一則註解"), 4000);
        return;
    }
    if (!controller_->info().permissions.copy) {
        statusBar()->showMessage(tr("此文件的權限設定不允許複製內容"), 5000);
        return;
    }

    const std::size_t index = annotationOrder_[static_cast<std::size_t>(row)];
    const auto& items = controller_->annotations();
    if (index >= items.size()) return;
    const domain::AnnotationSummary& target = items[index];

    // 摘要不夠：貼上需要完整幾何。從檔案讀回那一則，再挑出頁碼與頁內序號
    // 相符的——與刪除、編輯註釋用的是同一組定位座標。
    QString error;
    const std::vector<app::XfdfEntry> all =
        annotations_->readAnnotationsForExport(currentPath_, &error);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error, 6000);
        return;
    }

    std::vector<app::XfdfEntry> selected;
    for (const app::XfdfEntry& entry : all) {
        if (entry.pageIndex != target.pageIndex) continue;
        if (entry.annotation.rect.normalized() == domain::RectF{target.rect}.normalized()) {
            selected.push_back(entry);
            break;
        }
    }
    if (selected.empty()) {
        statusBar()->showMessage(tr("這一則註解的型別目前不支援複製"), 5000);
        return;
    }

    QMimeData* mime =
        app::makeAnnotationMimeData(selected, QFileInfo(currentPath_).fileName());
    if (mime == nullptr) return;
    QApplication::clipboard()->setMimeData(mime);
    statusBar()->showMessage(tr("已複製 1 則註解"), 4000);
}

void MainWindow::pasteAnnotations() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) return;
    if (!controller_->info().permissions.annotate) {
        statusBar()->showMessage(tr("此文件的權限設定不允許加註"), 5000);
        return;
    }

    std::vector<app::XfdfEntry> entries =
        app::annotationsFromMimeData(QApplication::clipboard()->mimeData());
    if (entries.empty()) return;  // 剪貼簿上不是註解，交給其他貼上路徑

    const int targetPage = pageView_->pageIndex();
    const domain::SizeF targetSize = controller_->pageSizePt(targetPage);
    for (app::XfdfEntry& entry : entries) {
        const bool samePage = entry.pageIndex == targetPage;
        const domain::SizeF sourceSize =
            samePage ? targetSize : controller_->pageSizePt(entry.pageIndex);
        // 同頁貼上要偏移一點，否則貼上結果完全疊在來源正上方，看起來像沒動作。
        // 跨頁／跨文件則以左上角為錨點換算（Acrobat 的慣例，貼到較小的頁面
        // 時仍然對得上，而不是憑空按頁面中心對齊）。
        entry.annotation.rect =
            samePage ? domain::pasteOffsetSamePage(entry.annotation.rect)
                     : domain::pasteOffsetForPage(entry.annotation.rect, sourceSize, targetSize);
        entry.pageIndex = targetPage;
        // 貼上的是一則新註解，不是同一則：沿用來源的 /NM 會讓兩則註解共用
        // 同一個唯一 ID，回覆串與外部工具會把它們當成同一則。
        entry.annotation = app::AnnotationService::stamped(std::move(entry.annotation),
                                                           annotationAuthor());
    }

    if (!confirmNoExternalChange(tr("貼上註解"))) return;

    const QString path = currentPath_;
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("貼上註解");
    command.redo = [this, path, entries, state] {
        *state = annotations_->addAnnotations(path, entries);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        statusBar()->showMessage(state->message.isEmpty() ? tr("貼上失敗") : state->message, 8000);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::deleteSelectedAnnotation() {
    const auto& items = controller_->annotations();
    // 頁面上的選取優先於清單的目前列：用「選取註解」工具點中的那一則可能
    // 被清單目前的篩選條件濾掉，這時清單的目前列指的是別則註解——
    // 刪掉它會是一個完全無法預期的結果。
    std::size_t index = selectedAnnotation_;
    if (index >= items.size()) {
        const int row = annotationList_->currentRow();
        if (row < 0 || row >= static_cast<int>(annotationOrder_.size())) return;
        index = annotationOrder_[static_cast<std::size_t>(row)];
    }
    if (index >= items.size()) return;
    if (!confirmNoExternalChange(tr("刪除註解"))) return;

    const domain::AnnotationSummary target = items[index];
    const QString path = currentPath_;

    // 與其他文件修改一樣包成命令物件（CLAUDE.md）。復原是把檔案截回原長度——
    // 刪除只是把參照從 /Annots 拿掉，註解物件本身還在，所以截回去就完整還原了。
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = tr("刪除註解");
    command.redo = [this, path, target, state] {
        *state = annotations_->deleteAnnotation(path, target.pageIndex, target.indexOnPage);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("刪除註解"),
                             state->message.isEmpty() ? tr("刪除失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    fileSnapshot_ = platform::captureSnapshot(path);
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::copySelectionToClipboard() {
    const app::Selection& current = selection_->selection();
    if (current.isEmpty() || current.text.isEmpty()) {
        statusBar()->showMessage(tr("沒有選取任何文字"), 4000);
        return;
    }
    if (controller_->isOpen() && !controller_->info().permissions.copy) {
        // 權限旗標在這裡也要擋。動作已經灰化，但快捷鍵仍然打得到——
        // 只灰化按鈕等於沒擋（PRD-SEC-001）。
        QMessageBox::warning(this, tr("複製"), tr("此文件的權限設定不允許複製內容"));
        return;
    }

    // 純文字與富文字兩種格式都放進剪貼簿（PRD-TXT-002）。
    //
    // 富文字不是為了字型或顏色——PDF 的文字層沒有可靠的樣式資訊——而是為了
    // **保住換行結構**。純文字貼進文書處理器之後，跨頁與跨段的分行常常被當成
    // 自動換行而重排，長引文會糊成一整段。
    auto* data = new QMimeData;
    data->setText(current.text);

    QString html = QStringLiteral("<div>");
    for (const QString& paragraph : current.text.split(QLatin1Char('\n'))) {
        html += QStringLiteral("<p>") + paragraph.toHtmlEscaped() + QStringLiteral("</p>");
    }
    html += QStringLiteral("</div>");
    data->setHtml(html);
    QApplication::clipboard()->setMimeData(data);

    if (current.isMultiPage()) {
        statusBar()->showMessage(tr("已複製 %1 個字元（跨 %2 頁）")
                                     .arg(current.text.size())
                                     .arg(current.pages.size()),
                                 5000);
    } else {
        statusBar()->showMessage(tr("已複製 %1 個字元").arg(current.text.size()), 4000);
    }
}

void MainWindow::copyAreaTextAsTsv(int pageIndex, const domain::RectF& pageRect) {
    // 表格切欄靠幾何（列看 y 重疊、欄看 x 自然斷點），不假設 PDF 裡有表格結構——
    // 絕大多數 PDF 沒有，而假設有的實作會在真實文件上整個失效。
    auto extractor = std::make_shared<engine::text::TextExtractor>();
    auto* window = this;
    extractor->open(currentPath_.toStdString(), "",
                    [extractor, window, pageIndex, pageRect](domain::DocumentError error) {
                        if (error != domain::DocumentError::None) return;
                        extractor->withTextPage(
                            pageIndex, [extractor, window, pageRect](
                                           const engine::text::TextPage* page) {
                                if (page == nullptr) return;
                                const std::string tsv =
                                    domain::extractTsv(page->layer(), pageRect);
                                QMetaObject::invokeMethod(
                                    window,
                                    [extractor, window, tsv] {
                                        if (tsv.empty()) {
                                            window->statusBar()->showMessage(
                                                tr("框選範圍內沒有文字"), 4000);
                                            return;
                                        }
                                        QApplication::clipboard()->setText(
                                            QString::fromStdString(tsv));
                                        window->statusBar()->showMessage(
                                            tr("已複製為 TSV（可直接貼進試算表）"), 4000);
                                    },
                                    Qt::QueuedConnection);
                            });
                    });
}

void MainWindow::copySnapshotToClipboard(int pageIndex, const domain::RectF& pageRect) {
    QFile source(currentPath_);
    if (!source.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(tr("無法讀取目前的文件"), 5000);
        return;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    engine::enhance::SnapshotArea area;
    area.left = pageRect.left;
    area.top = pageRect.bottom;
    area.right = pageRect.right;
    area.bottom = pageRect.top;

    // 以目前的檢視倍率換算 dpi，讓截下來的影像與畫面上看到的細節一致。
    // 固定 150 dpi 會讓放大檢視時截到比畫面糊的圖，那正是使用者用快照的時機。
    const double dpi = std::clamp(72.0 * pageView_->scale(), 72.0, 600.0);
    const engine::enhance::SnapshotResult result = engine::enhance::renderSnapshot(
        std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())), pageIndex, area,
        dpi);
    if (!result.ok) {
        statusBar()->showMessage(tr("快照失敗：%1").arg(QString::fromStdString(result.diagnostic)),
                                 6000);
        return;
    }

    // PixelBuffer 是 BGRA premultiplied，與 QImage 的 Format_ARGB32_Premultiplied 相同；
    // stride 必須用 buffer 自己的，不能假設寬×4（CLAUDE.md 硬性限制 3 的同一個坑）。
    const QImage image(result.pixels.data(), result.pixels.width(), result.pixels.height(),
                       static_cast<qsizetype>(result.pixels.stride()),
                       QImage::Format_ARGB32_Premultiplied);
    QApplication::clipboard()->setImage(image.copy());
    statusBar()->showMessage(tr("已複製快照（%1 × %2）")
                                 .arg(result.pixels.width())
                                 .arg(result.pixels.height()),
                             4000);
}

void MainWindow::navigateToPage(int pageIndex) {
    if (pageView_ == nullptr) return;
    // 離開之前先把「現在在哪」記下來，否則第一次按後退會沒有東西可退回——
    // 使用者從第 1 頁點書籤跳到第 80 頁之後，最想做的就是退回第 1 頁。
    viewHistory_.record(app::ViewPosition{pageView_->pageIndex(), pageView_->scale()});
    viewHistory_.record(app::ViewPosition{pageIndex, pageView_->scale()});
    pageView_->setPageIndex(pageIndex);
    updateNavigationActions();
}

void MainWindow::applyPermissionRestrictions() {
    // PRD-SEC-001：權限旗標要反映在介面上，不是等使用者按下去才拒絕。
    //
    // 灰化而不是隱藏：隱藏會讓使用者以為這個產品沒有這個功能，然後跑去找
    // 別的工具；灰化加上 tooltip 說明才傳達得出「是這份文件不允許」。
    //
    // **這不是安全機制。** 權限旗標只有在文件加密時才有強制力，而任何人都能
    // 用別的工具把它拿掉。這裡做的是尊重文件作者的意圖，不是防止規避——
    // 把它當成保護會讓人以為敏感內容真的被鎖住了。
    const domain::Permissions& permissions = controller_->info().permissions;
    const bool open = controller_->isOpen();

    const auto restrict = [this, open](const QString& actionId, bool allowed,
                                       const QString& reason) {
        QAction* action = actionRegistry_->action(actionId);
        if (action == nullptr) return;
        const bool usable = !open || allowed;
        action->setEnabled(usable);
        if (!usable) {
            action->setToolTip(reason);
        } else if (action->toolTip() == reason) {
            // 只清掉自己設的那一則。無條件清空會把別處設定的說明也一起洗掉。
            action->setToolTip(QString());
        }
    };

    const QString noPrint = tr("此文件的權限設定不允許列印");
    const QString noCopy = tr("此文件的權限設定不允許複製內容");
    const QString noAnnotate = tr("此文件的權限設定不允許加註");
    const QString noModify = tr("此文件的權限設定不允許修改");
    const QString noAssemble = tr("此文件的權限設定不允許重組頁面");
    const QString noForms = tr("此文件的權限設定不允許填寫表單");

    restrict(QStringLiteral("file.print"), permissions.print, noPrint);
    restrict(QStringLiteral("file.printPreview"), permissions.print, noPrint);
    restrict(QStringLiteral("edit.copy"), permissions.copy, noCopy);
    restrict(QStringLiteral("tool.snapshot"), permissions.copy, noCopy);
    restrict(QStringLiteral("tool.areaSelect"), permissions.copy, noCopy);
    restrict(QStringLiteral("file.exportText"), permissions.copy, noCopy);
    restrict(QStringLiteral("file.exportImage"), permissions.copy, noCopy);

    for (const char* id : {"annot.highlight", "annot.underline", "annot.strikeout",
                           "annot.stickyNote", "annot.rectangle", "annot.ellipse", "annot.line",
                           "annot.arrow", "annot.polygon", "annot.polyline", "annot.cloud",
                           "comment.delete"}) {
        restrict(QString::fromLatin1(id), permissions.annotate, noAnnotate);
    }

    for (const char* id : {"page.insert", "page.delete", "page.rotate", "page.move",
                           "page.labels", "document.merge", "document.split"}) {
        restrict(QString::fromLatin1(id), permissions.assemble, noAssemble);
    }

    restrict(QStringLiteral("protect.redactMark"), permissions.modify, noModify);
    restrict(QStringLiteral("protect.redactApply"), permissions.modify, noModify);
    restrict(QStringLiteral("form.resetForm"), permissions.fillForms, noForms);
    restrict(QStringLiteral("form.importData"), permissions.fillForms, noForms);

    // 縮圖拖曳重排是頁面重組。動作灰化擋不住拖曳，要關掉拖放本身。
    if (thumbnailList_ != nullptr) {
        thumbnailList_->setDragDropMode(open && !permissions.assemble
                                            ? QAbstractItemView::NoDragDrop
                                            : QAbstractItemView::InternalMove);
    }
}

void MainWindow::updateNavigationActions() {
    if (backAction_ != nullptr) backAction_->setEnabled(viewHistory_.canGoBack());
    if (forwardAction_ != nullptr) forwardAction_->setEnabled(viewHistory_.canGoForward());
}

void MainWindow::applyDefaultPanelLayout() {
    // 十三個面板全部同時展開，會把文件擠成視窗的不到一半——那是這個產品要看的東西。
    // 對標產品的預設也只開左側導覽一疊，其餘由使用者按需要叫出來。
    //
    // 這裡只設「首次啟動」的樣子。restoreSession() 之後若有存下來的版面，
    // restoreState() 會整份蓋掉本函式的結果，使用者自己排的版面不會被搶走。

    // 簽章面板原本加在左側卻沒有併入書籤那一疊，於是自成第二列，
    // 把左側切成上下兩塊。它和縮圖／書籤一樣是「文件的導覽結構」，同一疊即可。
    tabifyDockWidget(bookmarkDock_, signatureDock_);

    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        dock->setVisible(dock == thumbnailDock_);
    }
    thumbnailDock_->raise();

    // 導覽面板是輔助，不是主角：給一個夠看縮圖的寬度，剩下全部留給文件。
    resizeDocks({thumbnailDock_}, {280}, Qt::Horizontal);
}

std::vector<QWidget*> MainWindow::focusCycleTargets() const {
    std::vector<QWidget*> targets;
    // 檢視區永遠排第一。它是這個產品的主體，使用者按 F6 最常想回去的地方。
    if (pageView_ != nullptr) targets.push_back(pageView_);

    QList<QDockWidget*> docks = findChildren<QDockWidget*>();
    // 依畫面位置排序，而不是建立順序。焦點順序與視覺順序不一致是無障礙
    // 檢查最常見的失分項，而停靠面板的建立順序與最終排列幾乎不會一致
    // （tabify 與使用者拖曳都會改變它）。
    std::sort(docks.begin(), docks.end(), [](const QDockWidget* a, const QDockWidget* b) {
        const QPoint pa = a->pos();
        const QPoint pb = b->pos();
        if (pa.x() != pb.x()) return pa.x() < pb.x();
        return pa.y() < pb.y();
    });

    for (QDockWidget* dock : docks) {
        // 收合或被 tab 蓋住的面板跳過：把焦點送進看不見的東西，
        // 螢幕閱讀器會念出使用者眼前根本沒有的內容。
        if (!dock->isVisible()) continue;
        QWidget* content = dock->widget();
        if (content == nullptr) continue;
        targets.push_back(content->focusProxy() != nullptr ? content->focusProxy() : content);
    }
    return targets;
}

void MainWindow::focusNextPanel(bool backwards) {
    const std::vector<QWidget*> targets = focusCycleTargets();
    if (targets.empty()) return;

    QWidget* current = QApplication::focusWidget();
    // 目前焦點可能在面板內部的深層元件上，往上找它屬於哪一個目標。
    std::size_t index = 0;
    bool found = false;
    for (std::size_t i = 0; i < targets.size() && !found; ++i) {
        for (QWidget* node = current; node != nullptr; node = node->parentWidget()) {
            if (node == targets[i]) {
                index = i;
                found = true;
                break;
            }
        }
    }

    const std::size_t count = targets.size();
    const std::size_t next = found ? (backwards ? (index + count - 1) % count : (index + 1) % count)
                                   : 0;
    targets[next]->setFocus(backwards ? Qt::BacktabFocusReason : Qt::TabFocusReason);
}

void MainWindow::recordHistory(const QString& path) {
    history_.recordOpen(path, controller_->info().title.empty()
                                  ? QString()
                                  : QString::fromStdString(controller_->info().title),
                        controller_->info().pageCount);
    history_.save();
    historyPanel_->reload();
}

void MainWindow::storeReadingPosition() {
    if (currentPath_.isEmpty()) return;
    // 位置只在關檔或換檔時寫一次。每次捲動都寫等於在使用者閱讀時持續碰硬碟。
    history_.updatePosition(currentPath_, pageView_->pageIndex(), pageView_->scale());
    history_.save();
}

void MainWindow::reloadNavigationPanels(const QString& path) {
    destinationsPanel_->setDocument(path);
    // 關檔一定要清；換檔只在面板開著時才重讀（收起來時由 visibilityChanged 補）。
    if (path.isEmpty() || (linksDock_ != nullptr && linksDock_->isVisible())) {
        linksPanel_->setPage(path.isEmpty() ? -1 : pageView_->pageIndex());
    }

    // 附件用物件層同步讀。整份檔案讀進記憶體在這裡是可接受的：
    // 附件面板只在使用者打開它時才有意義，而 PRD §8.1 的預算針對的是渲染路徑。
    attachmentsPanel_->setAttachments({});
    tagsPanel_->clearDocument();
    orderPanel_->clearDocument();
    inspectorPanel_->clearDocument();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = file.readAll();
    file.close();
    engine::objects::PdfSourceDocument source;
    if (source.open(std::string(raw.constData(), static_cast<std::size_t>(raw.size()))) !=
        engine::objects::SourceStatus::Ok) {
        return;
    }
    attachmentsPanel_->setAttachments(engine::attachments::listAttachments(source));
    // 標籤結構（PRD-A11Y-001）。與附件共用同一次檔案讀取與同一份
    // PdfSourceDocument——這兩件事都只在使用者打開對應面板時才有意義，
    // 各讀一次檔會讓開檔多花一整份 I/O。
    const engine::objects::StructTree tree = engine::objects::readStructTree(source);
    tagsPanel_->setStructTree(tree);
    // Order 面板（PRD-A11Y-002）與檢查器（PRD-A11Y-003）復用同一份結構樹，
    // 不再重新解析一次。Order 面板還需要目前頁碼才能算這一頁的順序比對。
    orderPanel_->setStructTree(tree);
    orderPanel_->setPageIndex(pageView_->pageIndex());
    inspectorPanel_->setReport(engine::objects::runAccessibilityCheck(source, tree));
}

void MainWindow::saveAttachment(int index, const QString& suggestedName) {
    QFile file(currentPath_);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = file.readAll();
    file.close();

    engine::objects::PdfSourceDocument source;
    if (source.open(std::string(raw.constData(), static_cast<std::size_t>(raw.size()))) !=
        engine::objects::SourceStatus::Ok) {
        statusBar()->showMessage(tr("無法解析文件結構"), 5000);
        return;
    }
    const std::vector<engine::attachments::Attachment> list =
        engine::attachments::listAttachments(source);
    if (index < 0 || index >= static_cast<int>(list.size())) return;

    const engine::attachments::ExtractResult extracted =
        engine::attachments::extractAttachment(source, list[static_cast<std::size_t>(index)]);
    if (!extracted.ok) {
        // 把沒解開的位元組寫出去，使用者拿到的是垃圾而且無從得知。
        statusBar()->showMessage(tr("無法取出附件：%1")
                                     .arg(QString::fromStdString(extracted.diagnostic)),
                                 8000);
        return;
    }

    const QString target = QFileDialog::getSaveFileName(this, tr("另存附件"), suggestedName);
    if (target.isEmpty()) return;
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage(tr("無法寫入 %1").arg(target), 5000);
        return;
    }
    out.write(extracted.bytes.data(), static_cast<qint64>(extracted.bytes.size()));
    out.close();
    statusBar()->showMessage(tr("已儲存至 %1").arg(target), 5000);
}

void MainWindow::applyShortcutScheme() {
    // 鍵位表是資料，動作是物件，兩者靠 id 對起來。表裡有而程式沒有的 id
    // 靜靜跳過就好——那代表設定檔比程式新，不是錯誤；反過來（程式有而表裡
    // 沒有）則保留動作自己在建立時設的預設鍵，不清掉。
    for (const app::ShortcutBinding& binding : shortcuts_.bindings()) {
        QAction* action = actionRegistry_->action(binding.actionId);
        if (action == nullptr) continue;
        action->setShortcut(binding.sequence);
    }
}

void MainWindow::toggleAutoScroll() {
    autoScroller_.toggle();
    if (autoScroller_.active()) {
        autoScrollTimer_->start(16);
        statusBar()->showMessage(tr("自動捲動：第 %1 級（Esc 停止）").arg(autoScroller_.speed()),
                                 3000);
    } else {
        autoScrollTimer_->stop();
        statusBar()->showMessage(tr("自動捲動已停止"), 3000);
    }
}

void MainWindow::populateAnnotations() {
    const auto& items = controller_->annotations();

    // 下拉選單的內容跟著文件走。重建時擋掉訊號，否則 clear() 會觸發
    // currentIndexChanged，於是 populateAnnotations 遞迴呼叫自己。
    const QString keptAuthor = annotationAuthorFilter_->currentData().toString();
    const QString keptType = annotationTypeFilter_->currentData().toString();
    {
        const QSignalBlocker blockAuthor(annotationAuthorFilter_);
        const QSignalBlocker blockType(annotationTypeFilter_);
        annotationAuthorFilter_->clear();
        annotationAuthorFilter_->addItem(tr("所有作者"), QString());
        for (const std::string& author : domain::distinctAuthors(items)) {
            annotationAuthorFilter_->addItem(QString::fromStdString(author),
                                             QString::fromStdString(author));
        }
        annotationTypeFilter_->clear();
        annotationTypeFilter_->addItem(tr("所有類型"), QString());
        for (const std::string& subtype : domain::distinctSubtypes(items)) {
            annotationTypeFilter_->addItem(QString::fromStdString(subtype),
                                           QString::fromStdString(subtype));
        }
        // 盡量保住使用者原本選的篩選條件。文件重新載入之後那個作者可能不見了，
        // 找不到就退回「所有」——靜靜換成另一個作者會更糟。
        const int authorIndex = annotationAuthorFilter_->findData(keptAuthor);
        annotationAuthorFilter_->setCurrentIndex(authorIndex >= 0 ? authorIndex : 0);
        const int typeIndex = annotationTypeFilter_->findData(keptType);
        annotationTypeFilter_->setCurrentIndex(typeIndex >= 0 ? typeIndex : 0);
    }

    domain::AnnotationFilter filter;
    filter.author = annotationAuthorFilter_->currentData().toString().toStdString();
    filter.subtype = annotationTypeFilter_->currentData().toString().toStdString();
    filter.text = annotationSearch_->text().toStdString();

    const auto key =
        static_cast<domain::AnnotationSortKey>(annotationSortKey_->currentData().toInt());
    annotationOrder_ = domain::filterAndSort(items, filter, key, true);

    annotationList_->clear();
    for (const std::size_t index : annotationOrder_) {
        const domain::AnnotationSummary& item = items[index];
        QString text = tr("第 %1 頁 · %2").arg(item.pageIndex + 1)
                           .arg(QString::fromStdString(item.subtype));
        if (!item.author.empty()) {
            text += tr(" · %1").arg(QString::fromStdString(item.author));
        }
        if (!item.contents.empty()) {
            text += tr("：%1").arg(QString::fromStdString(item.contents));
        }
        new QListWidgetItem(text, annotationList_);
    }

    // 篩掉全部時要說出來。空白清單與「這份文件沒有註解」在畫面上看起來一樣，
    // 而使用者往往忘了自己還開著篩選條件。
    if (annotationOrder_.empty() && !items.empty()) {
        auto* empty = new QListWidgetItem(tr("目前的篩選條件沒有符合的註解"), annotationList_);
        empty->setFlags(Qt::NoItemFlags);
    }
}

void MainWindow::runSearch() {
    const QString query = searchField_->text();
    searchResults_->clear();
    if (query.isEmpty()) return;
    statusBar()->showMessage(tr("搜尋中…"));
    selection_->search(query, pageView_->pageIndex(), false, false);
}

void MainWindow::populateOutline() {
    outlineTree_->clear();

    // 書籤樹的深度來自不可信任的輸入，引擎端已限制在 32 層；
    // 這裡用迭代建樹，避免在 UI 執行緒上遞迴爆堆疊。
    struct Pending {
        const domain::OutlineNode* node;
        QTreeWidgetItem* parent;
    };

    std::vector<Pending> stack;
    const auto& roots = controller_->outline();
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        stack.push_back(Pending{&*it, nullptr});
    }

    while (!stack.empty()) {
        const Pending pending = stack.back();
        stack.pop_back();

        auto* item = pending.parent ? new QTreeWidgetItem(pending.parent)
                                    : new QTreeWidgetItem(outlineTree_);
        item->setText(0, QString::fromStdString(pending.node->title));
        if (pending.node->pageIndex) {
            item->setData(0, Qt::UserRole, *pending.node->pageIndex);
            item->setToolTip(0, tr("第 %1 頁").arg(*pending.node->pageIndex + 1));
        }

        for (auto it = pending.node->children.rbegin(); it != pending.node->children.rend(); ++it) {
            stack.push_back(Pending{&*it, item});
        }
    }

    outlineTree_->expandToDepth(1);
}

void MainWindow::populateThumbnails() {
    thumbnailList_->clear();
    const int pages = controller_->pageCount();
    for (int i = 0; i < pages; ++i) {
        auto* item = new QListWidgetItem(tr("%1").arg(i + 1), thumbnailList_);
        item->setTextAlignment(Qt::AlignHCenter);
    }

    // 只先要看得到的那幾張。整份文件一次要縮圖，在一萬頁的文件上等於自殺——
    // 縮圖任務即使是最低優先權，也會把佇列塞滿並延後可見圖磚。
    const int initial = std::min(pages, 24);
    for (int i = 0; i < initial; ++i) {
        controller_->requestThumbnail(i);
    }
}

void MainWindow::buildPanelActions() {
    // 面板的開關動作直接用 QDockWidget 自帶的 toggleViewAction：它已經正確處理
    // 「面板被關掉時勾選狀態要跟著變」，自己重寫一份必然會漏掉某個路徑。
    const auto bindPanel = [this](const QString& id, QDockWidget* dock) {
        if (!dock) return;
        registerRibbonAction(id, dock->toggleViewAction());
    };

    bindPanel(QStringLiteral("view.panel.thumbnails"), thumbnailDock_);
    bindPanel(QStringLiteral("view.panel.bookmarks"), bookmarkDock_);
    bindPanel(QStringLiteral("view.panel.comments"), commentDock_);
    bindPanel(QStringLiteral("view.panel.search"), searchDock_);
    // Ribbon 的預設配置用的是功能導向的名字（「註解清單」「書籤」「進階搜尋」），
    // 而面板開關本來就是同一件事——各自再做一顆按鈕只會讓兩顆按鈕互相搶狀態。
    if (commentDock_ != nullptr) {
        registerRibbonAction(QStringLiteral("comment.list"), commentDock_->toggleViewAction());
    }
    if (bookmarkDock_ != nullptr) {
        registerRibbonAction(QStringLiteral("bookmark.manage"), bookmarkDock_->toggleViewAction());
    }
    if (searchDock_ != nullptr) {
        registerRibbonAction(QStringLiteral("search.advanced"), searchDock_->toggleViewAction());
    }
    bindPanel(QStringLiteral("view.panel.signatures"), signatureDock_);
    bindPanel(QStringLiteral("view.panel.history"), historyDock_);
    bindPanel(QStringLiteral("view.panel.destinations"), destinationsDock_);
    bindPanel(QStringLiteral("view.panel.links"), linksDock_);
    bindPanel(QStringLiteral("view.panel.attachments"), attachmentsDock_);
    bindPanel(QStringLiteral("view.panel.layers"), layersDock_);
    bindPanel(QStringLiteral("view.panel.fields"), fieldsDock_);
    bindPanel(QStringLiteral("view.panel.properties"), propertiesDock_);
    // 這三個先前沒有開關動作。面板全部預設展開時「反正看得到」還撐得住，
    // 改成預設收起來之後，沒有開關就等於功能消失。
    bindPanel(QStringLiteral("view.panel.tags"), tagsDock_);
    bindPanel(QStringLiteral("view.panel.order"), orderDock_);
    bindPanel(QStringLiteral("view.panel.inspector"), inspectorDock_);
    bindPanel(QStringLiteral("view.panel.panZoom"), panZoomDock_);
    bindPanel(QStringLiteral("view.panel.loupe"), loupeDock_);

    // 主題（PRD-UI-006）。色票是資料，套用是把它變成 QPalette 交給 qApp——
    // 不逐個 widget 設 styleSheet，那種寫法會讓「深色模式下這個面板忘了改」
    // 變成常態。
    connect(theme_, &app::ThemeController::themeChanged, this,
            [this](app::ThemeMode) { qApp->setPalette(theme_->currentPalette().toQPalette()); });

    auto* themeMenu = new QMenu(tr("主題"), this);
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    const auto addTheme = [&](const QString& text, app::ThemeMode mode) {
        auto* action = themeMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(theme_->mode() == mode);
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { theme_->setMode(mode); });
        return action;
    };
    registerRibbonAction(QStringLiteral("view.theme.system"),
                         addTheme(tr("跟隨系統"), app::ThemeMode::System));
    registerRibbonAction(QStringLiteral("view.theme.light"),
                         addTheme(tr("淺色"), app::ThemeMode::Light));
    registerRibbonAction(QStringLiteral("view.theme.dark"),
                         addTheme(tr("深色"), app::ThemeMode::Dark));
    menuBar()->addMenu(themeMenu);
    qApp->setPalette(theme_->currentPalette().toQPalette());

    // 使用者自訂的鍵位（PRD-UI-004）。整批不合就整批不套並落回預設——
    // 「盡量套用」會讓使用者分不出哪幾個鍵其實沒生效。
    const QByteArray storedShortcuts = settings_->shortcutsJson();
    if (!storedShortcuts.isEmpty() && !shortcuts_.importFromJson(storedShortcuts).ok) {
        shortcuts_.resetToDefaults();
    }

    // 鍵位表最後才套：動作是分階段註冊的（面板開關在這個函式裡才註冊），
    // 太早套會漏掉後註冊的那些。
    applyShortcutScheme();

    // 註解屬性側邊欄（PRD-ANN-009）。Ctrl+' 對齊 Acrobat。
    auto* propertiesAction = new QAction(tr("註解屬性"), this);
    propertiesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+'")));
    connect(propertiesAction, &QAction::triggered, this, [this] {
        propertiesDock_->show();
        propertiesDock_->raise();
    });
    addAction(propertiesAction);
    registerRibbonAction(QStringLiteral("annot.properties"), propertiesAction);
    bindPanel(QStringLiteral("view.panel.tags"), tagsDock_);

    // F6 / Shift+F6 在面板之間循環（PRD-A11Y-005）。
    //
    // 用 ApplicationShortcut 而不是預設的 WindowShortcut context：焦點若在
    // 面板內部的樹狀元件上，WindowShortcut 仍然會送達，但一旦有子視窗
    // （例如浮動的停靠面板）就會斷掉，而浮動面板正是最需要 F6 的情境。
    const auto addCycleKey = [this](const QKeySequence& key, bool backwards) {
        auto* action = new QAction(backwards ? tr("上一個面板") : tr("下一個面板"), this);
        action->setShortcut(key);
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, [this, backwards] { focusNextPanel(backwards); });
        addAction(action);
    };
    addCycleKey(QKeySequence(Qt::Key_F6), false);
    addCycleKey(QKeySequence(Qt::SHIFT | Qt::Key_F6), true);

    // 自動捲動（PRD-NAV-008）。快捷鍵對齊 Acrobat 的 Ctrl+Shift+H。
    // 速度與方向的調整只在捲動進行中有意義，因此不另外進 Ribbon。
    autoScrollTimer_ = new QTimer(this);
    connect(autoScrollTimer_, &QTimer::timeout, this, [this] {
        const int delta = autoScroller_.tick(16.0);
        if (delta == 0) return;
        if (!pageView_->scrollByPixels(delta)) {
            // 捲到底了。停下來而不是繼續空轉——持續觸發 timer 只是耗電。
            autoScroller_.stop();
            autoScrollTimer_->stop();
            statusBar()->showMessage(tr("已到文件結尾，自動捲動停止"), 3000);
        }
    });

    auto* autoScrollAction = new QAction(tr("自動捲動"), this);
    autoScrollAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    connect(autoScrollAction, &QAction::triggered, this, [this] { toggleAutoScroll(); });
    addAction(autoScrollAction);
    registerRibbonAction(QStringLiteral("view.autoscroll"), autoScrollAction);

    const auto addScrollKey = [this](const QKeySequence& key, std::function<void()> run) {
        auto* action = new QAction(this);
        action->setShortcut(key);
        connect(action, &QAction::triggered, this, [run = std::move(run)] { run(); });
        addAction(action);
    };
    addScrollKey(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up),
                 [this] { autoScroller_.increaseSpeed(); });
    addScrollKey(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down),
                 [this] { autoScroller_.decreaseSpeed(); });
    addScrollKey(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Left),
                 [this] { autoScroller_.toggleDirection(); });

    // 使用者書籤（PRD-NAV-005）。存在使用者端而不寫進文件——
    // 審閱者對唯讀或已簽章文件也要能做標記。
    auto* markAction = new QAction(tr("加入閱讀書籤"), this);
    markAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    connect(markAction, &QAction::triggered, this, [this] {
        if (currentPath_.isEmpty()) return;
        alioth::app::ReadingMark mark;
        mark.pageIndex = pageView_->pageIndex();
        if (!history_.addMark(currentPath_, mark)) return;
        history_.save();
        historyPanel_->reload();
        statusBar()->showMessage(tr("已標記第 %1 頁").arg(mark.pageIndex + 1), 3000);
    });
    addAction(markAction);
    registerRibbonAction(QStringLiteral("nav.mark"), markAction);
    registerRibbonAction(QStringLiteral("bookmark.add"), markAction);

    auto* findAction = new QAction(tr("尋找"), this);
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this] {
        searchDock_->show();
        searchDock_->raise();
        searchField_->setFocus();
        searchField_->selectAll();
    });
    addAction(findAction);
    registerRibbonAction(QStringLiteral("search.find"), findAction);

    auto* findNextAction = new QAction(tr("找下一個"), this);
    findNextAction->setShortcut(QKeySequence::FindNext);
    connect(findNextAction, &QAction::triggered, this, [this] {
        const int next = searchResults_->currentRow() + 1;
        if (next < searchResults_->count()) searchResults_->setCurrentRow(next);
    });
    addAction(findNextAction);
    registerRibbonAction(QStringLiteral("search.findNext"), findNextAction);
}

void MainWindow::updateGeometryLabel(int pageIndex, const domain::PointF* pagePoint) {
    if (geometryLabel_ == nullptr) return;
    if (!controller_->isOpen() || pageIndex < 0 || pageIndex >= controller_->pageCount()) {
        geometryLabel_->clear();
        return;
    }

    const domain::SizeF size = controller_->pageSizePt(pageIndex);
    // 尚未問到真實尺寸時什麼都不顯示，而不是顯示 A4 佔位值——
    // 顯示佔位值等於告訴使用者「這頁是 A4」，而那句話可能是錯的。
    if (!controller_->pageGeometryKnown(pageIndex)) {
        geometryLabel_->setText(tr("尺寸載入中"));
        return;
    }

    // 毫米是工程圖與紙張規格通用的單位；點只有排版的人在看。
    // 兩個都給，因為 PDF 的座標本身是點，量測與註解幾何都用它。
    const double mmPerPt = 25.4 / 72.0;
    const QString paper = tr("%1 × %2 mm")
                              .arg(size.width * mmPerPt, 0, 'f', 1)
                              .arg(size.height * mmPerPt, 0, 'f', 1);
    if (pagePoint == nullptr) {
        geometryLabel_->setText(paper);
        return;
    }
    geometryLabel_->setText(tr("%1　游標 %2, %3 pt")
                                .arg(paper)
                                .arg(pagePoint->x, 0, 'f', 1)
                                .arg(pagePoint->y, 0, 'f', 1));
}

void MainWindow::buildStatusBar() {
    pageLabel_ = new QLabel(tr("尚未開啟文件"), this);
    pageLabel_->setObjectName(QStringLiteral("statusPageLabel"));
    pageLabel_->setAccessibleName(tr("目前頁碼"));

    zoomLabel_ = new QLabel(QStringLiteral("100%"), this);
    zoomLabel_->setObjectName(QStringLiteral("statusZoomLabel"));
    // 「100%」本身沒有說明它是什麼。螢幕閱讀器念出裸數字時，
    // 使用者無法分辨那是縮放、頁碼還是進度。
    zoomLabel_->setAccessibleName(tr("縮放比例"));

    // 頁面尺寸與游標位置（PDF-XChange 的 Show Page Size / Position）。
    //
    // 工程圖審閱時這兩個數字一直被用到：核對紙張是不是 A0、量一段距離
    // 之前先確認座標。放在狀態列而不是浮動提示，是因為它要能被一直看著，
    // 而浮動提示會跟著游標動、遮住正在看的東西。
    geometryLabel_ = new QLabel(this);
    geometryLabel_->setObjectName(QStringLiteral("statusGeometryLabel"));
    geometryLabel_->setAccessibleName(tr("頁面尺寸與游標位置"));

    noticeLabel_ = new QLabel(this);
    noticeLabel_->setObjectName(QStringLiteral("statusNoticeLabel"));
    // 降級提示（XFA / JavaScript / 簽章）常駐在這裡。它是 PRD §13
    // 「不得誤導使用者」的載體，必須讓螢幕閱讀器讀得到。
    noticeLabel_->setAccessibleName(tr("文件相容性提示"));

    statusBar()->addWidget(pageLabel_);
    statusBar()->addPermanentWidget(noticeLabel_);
    statusBar()->addPermanentWidget(geometryLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);
}

void MainWindow::setSplitMode(SplitMode mode) {
    if (mode == splitMode_) return;

    // 每次切換都先拆乾淨再重建。
    //
    // 想「從兩格加一格變成四格」會需要在既有的分割器之間搬 widget，而
    // QSplitter 對重新插入的順序與伸展比例有自己的記憶，搬出來的版面常常
    // 不是預期的位置。整個重建的成本只是幾個 widget，換來的是狀態只有一種。
    for (PageView* view : extraViews_) {
        if (view != nullptr) view->deleteLater();
    }
    extraViews_.clear();
    secondView_ = nullptr;
    if (topRow_ != nullptr) {
        topRow_->deleteLater();
        topRow_ = nullptr;
    }
    if (bottomRow_ != nullptr) {
        bottomRow_->deleteLater();
        bottomRow_ = nullptr;
    }
    // 主檢視不能被連帶刪掉——它是整個視窗唯一保有捲動位置與選取的那一個。
    pageView_->setParent(nullptr);

    splitMode_ = mode;

    // 兩個以上的檢視共用同一個控制器（也就是同一份文件把手與圖磚快取），
    // 但各自持有版面與縮放。共用快取是刻意的：看同一頁時圖磚只算一次。
    //
    // 已知取捨：任一邊捲動都會取消其他邊尚未開始的預取。可見圖磚不受影響，
    // 所以症狀最多是預取白費，不會畫錯。
    const auto makeView = [this] {
        auto* view = new PageView(controller_, selection_, this);
        view->setPageIndex(pageView_->pageIndex());
        view->setCursorSizeLevel(settings_->cursorSizeLevel());
        extraViews_.push_back(view);
        return view;
    };

    switch (mode) {
        case SplitMode::None:
            splitter_->setOrientation(Qt::Horizontal);
            splitter_->addWidget(pageView_);
            break;
        case SplitMode::Horizontal:
        case SplitMode::Vertical:
            splitter_->setOrientation(mode == SplitMode::Horizontal ? Qt::Vertical
                                                                    : Qt::Horizontal);
            splitter_->addWidget(pageView_);
            secondView_ = makeView();
            splitter_->addWidget(secondView_);
            break;
        case SplitMode::Quad:
            // 外層垂直、兩排水平：四格共用一條橫線與兩條直線。
            splitter_->setOrientation(Qt::Vertical);
            topRow_ = new QSplitter(Qt::Horizontal, this);
            bottomRow_ = new QSplitter(Qt::Horizontal, this);
            topRow_->addWidget(pageView_);
            secondView_ = makeView();
            topRow_->addWidget(secondView_);
            bottomRow_->addWidget(makeView());
            bottomRow_->addWidget(makeView());
            splitter_->addWidget(topRow_);
            splitter_->addWidget(bottomRow_);
            // 兩排的直向分隔線連動，否則四格會歪成階梯——那正是
            // 「試算表式分割」與「隨便切四塊」的差別。
            connect(topRow_, &QSplitter::splitterMoved, bottomRow_,
                    [this](int, int) { bottomRow_->setSizes(topRow_->sizes()); });
            connect(bottomRow_, &QSplitter::splitterMoved, topRow_,
                    [this](int, int) { topRow_->setSizes(bottomRow_->sizes()); });
            break;
    }

    pageView_->show();
    for (PageView* view : extraViews_) {
        view->fitWidth();
        view->show();
    }
}

void MainWindow::showPrintPreview() {
    if (!controller_->isOpen()) {
        QMessageBox::information(this, tr("列印預覽"), tr("尚未開啟文件"));
        return;
    }
    if (!controller_->info().permissions.print) {
        QMessageBox::warning(this, tr("列印預覽"), tr("此文件的權限設定不允許列印"));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QPrintPreviewDialog preview(&printer, this);
    preview.setWindowTitle(tr("列印預覽"));

    // 預覽與實際列印共用同一個 PrintService。兩條各自渲染的路徑會讓
    // 「預覽看到的」與「印出來的」慢慢分岔，而那種分岔要印出來才發現。
    //
    // 列印用自己的文件把手：PdfiumEngine 的把手屬於渲染執行緒，而列印是
    // 同步流程，混用就是在違反 PDFium 的執行緒限制。
    connect(&preview, &QPrintPreviewDialog::paintRequested, this,
            [this](QPrinter* target) {
                app::print::PrintService service;
                QString error;
                if (!service.openDocument(currentPath_, QString(), &error)) return;
                app::print::PrintOptions options;
                options.includeAnnotations = true;
                (void)service.print(*target, options);
            });
    preview.exec();
}

void MainWindow::printDocument() {
    if (!controller_->isOpen()) {
        QMessageBox::information(this, tr("列印"), tr("尚未開啟文件"));
        return;
    }
    if (!controller_->info().permissions.print) {
        // 權限旗標要在動作發生前就擋下並說明，不是印到一半才失敗（PRD-SEC-001）。
        QMessageBox::warning(this, tr("列印"), tr("此文件的權限設定不允許列印"));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("列印"));
    if (dialog.exec() != QDialog::Accepted) return;

    // 列印用自己的文件把手：PdfiumEngine 的把手屬於渲染執行緒，
    // 而列印是同步流程，兩者混用就是在違反 PDFium 的執行緒限制。
    app::print::PrintService service;
    QString error;
    if (!service.openDocument(currentPath_, QString(), &error)) {
        QMessageBox::warning(this, tr("列印"), error.isEmpty() ? tr("無法開啟文件") : error);
        return;
    }

    app::print::PrintOptions options;
    options.includeAnnotations = true;

    const app::print::PrintResult result = service.print(printer, options);
    if (!result.ok) {
        QMessageBox::warning(this, tr("列印"), result.message);
        return;
    }
    statusBar()->showMessage(tr("已送出 %1 張").arg(result.sheetsPrinted), 5000);
}

void MainWindow::registerRibbonAction(const QString& id, QAction* action) {
    actionRegistry_->registerAction(id, action);
}

void MainWindow::buildRibbon() {
    ribbon_ = new ribbon::RibbonBar(actionRegistry_, this);

    // 使用者自訂的配置優先；壞掉或不存在就退回預設，而不是顯示一個空的 Ribbon。
    const QString layoutPath = platform::configDirectory() + QStringLiteral("/ribbon.json");
    ribbon::ParseResult parse;
    ribbon::Layout layout = ribbon::loadFromFile(layoutPath, &parse);
    if (!parse.error.isEmpty() || layout.pages.isEmpty()) {
        layout = ribbon::defaultLayout();
    }
    ribbon_->setLayoutModel(layout);

    // Ribbon 放進頂部工具列區，不是中央版面。
    //
    // 放在中央版面時它只能橫跨「停靠面板之間剩下的那一段」——左側面板一開，
    // Ribbon 就從視窗中間才開始，右邊留一大塊空白。Ribbon 是全域命令列，
    // 視覺上必須橫跨整個視窗並壓在停靠區之上，而工具列區正是 QMainWindow
    // 唯一具備這個幾何的位置。
    //
    // 仍然不用 setMenuWidget()：那會刪掉 QMenuBar，連帶殺掉它擁有的所有 QAction
    // （本專案曾因此讓 46 個動作在啟動當下就被銷毀）。工具列與選單列可以並存，
    // 兩者共用同一組 QAction，切換只是顯示與隱藏。
    ribbonHost_ = new QToolBar(tr("Ribbon"), this);
    ribbonHost_->setObjectName(QStringLiteral("ribbonToolBar"));
    // 可移動或可浮動的 Ribbon 沒有意義，而且拖出去之後使用者往往找不回來。
    ribbonHost_->setMovable(false);
    ribbonHost_->setFloatable(false);
    ribbonHost_->setAllowedAreas(Qt::TopToolBarArea);
    ribbonHost_->setContentsMargins(0, 0, 0, 0);
    ribbonHost_->layout()->setContentsMargins(0, 0, 0, 0);
    ribbonHost_->addWidget(ribbon_);
    // 先斷行，讓 Ribbon 獨佔一列。否則它會跟既有的工具列擠在同一列，
    // Ribbon 從視窗中段才開始——那正是「按鈕分佈很亂」看起來的樣子。
    addToolBarBreak(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, ribbonHost_);
    addToolBarBreak(Qt::TopToolBarArea);
    // 工具列區在停靠區之上橫跨整個視窗寬度，需要角落也歸它管，
    // 否則左右停靠面板會把工具列切斷。
    setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);

    // 配置裡有但沒接上的動作會顯示為停用的佔位按鈕。啟動時把缺口寫進狀態列，
    // 讓「設定檔與程式版本脫節」在開發階段就被看見，而不是等使用者點到才發現。
    const QStringList missing = ribbon_->missingActionIds();
    if (!missing.isEmpty()) {
        statusBar()->showMessage(tr("Ribbon 有 %1 個動作尚未接上").arg(missing.size()), 4000);
    }

    auto* classicAction = new QAction(tr("使用傳統選單"), this);
    classicAction->setCheckable(true);
    connect(classicAction, &QAction::toggled, this, [this](bool on) { setRibbonVisible(!on); });
    registerRibbonAction(QStringLiteral("view.classicMenu"), classicAction);
    menuBar()->addAction(classicAction);

    // Ribbon 顯示時選單列收起來，但**不銷毀**——它擁有的動作還被工具列、
    // 快捷鍵與 Ribbon 自己引用著。
    setRibbonVisible(true);
}

void MainWindow::customizeRibbon() {
    if (ribbon_ == nullptr) return;

    RibbonCustomizeDialog dialog(ribbon_->layoutModel(), actionRegistry_, this);
    if (dialog.exec() != QDialog::Accepted) return;

    ribbon::Layout layout = dialog.result();
    // 使用者自己組的分頁不會有 KeyTip，補上之後 Alt 導覽才蓋得到新按鈕。
    ribbon::assignAutomaticKeyTips(layout);
    ribbon_->setLayoutModel(layout);

    // 存檔失敗只提示，不還原畫面：使用者剛做的自訂在這個 session 裡仍然有效，
    // 把它一起回退等於因為「存不起來」而多懲罰一次。
    const QString layoutPath = platform::configDirectory() + QStringLiteral("/ribbon.json");
    ribbon::ParseResult save;
    if (!ribbon::saveToFile(layout, layoutPath, &save)) {
        QMessageBox::warning(this, tr("自訂 Ribbon"),
                             tr("設定已套用，但無法寫入 %1：%2").arg(layoutPath, save.error));
        return;
    }
    statusBar()->showMessage(tr("已更新 Ribbon 配置"), 4000);
}

void MainWindow::setRibbonVisible(bool visible) {
    // 兩者互斥顯示，但都一直存在。共用同一組 QAction，因此不論目前顯示哪一個，
    // 快捷鍵與工具列都照常運作。
    // 顯示與隱藏的對象是承載 Ribbon 的工具列，不是 Ribbon 本身：
    // 只藏內層 widget 會留下一條工具列高度的空殼橫在視窗頂端。
    // 記下使用者的選擇。簡報模式會暫時藏掉 Ribbon，退出時要還原成
    // 「使用者原本選的樣子」，不是無條件顯示——不然切到傳統選單的使用者
    // 看完簡報之後會突然多出一條 Ribbon。
    ribbonWasVisible_ = visible;
    if (ribbonHost_ != nullptr) ribbonHost_->setVisible(visible);
    if (ribbon_ != nullptr) ribbon_->setVisible(visible);
    menuBar()->setVisible(!visible);
    // 檢視工具列的五顆按鈕在 Ribbon 的「檢視」分頁裡一顆不差地重複了一次。
    // 兩者同時出現，使用者要在兩個地方找同一個功能。
    if (viewToolBar_ != nullptr) viewToolBar_->setVisible(!visible);
}

void MainWindow::applySettings() {
    controller_->setCacheBytes(settings_->cacheBytes());
    controller_->setAutosaveSeconds(settings_->autosaveSeconds());
    controller_->setNightMode(settings_->nightMode());

    // PRD-UI-018：兩個檢視（Split View）各自持有游標，設定變更要同時套用，
    // 否則使用者切到第二個檢視會看到舊尺寸的準星。
    if (pageView_ != nullptr) pageView_->setCursorSizeLevel(settings_->cursorSizeLevel());
    for (PageView* view : extraViews_) {
        if (view != nullptr) view->setCursorSizeLevel(settings_->cursorSizeLevel());
    }

    // PRD-UI-013：觸控模式把 Ribbon 按鈕放大到 44 CSS px 等效。
    if (ribbon_ != nullptr) ribbon_->setTouchMode(settings_->touchMode());
}

void MainWindow::emailCurrentDocument() {
    if (currentPath_.isEmpty() || !controller_->isOpen()) {
        QMessageBox::information(this, tr("以電子郵件傳送"), tr("尚未開啟文件"));
        return;
    }

    app::DocumentEmailRequest request;
    request.documentPath = currentPath_;
    const platform::EmailComposeResult result =
        app::sendDocumentByEmail(request, platform::defaultEmailSender());
    if (!result.ok) {
        QMessageBox::warning(this, tr("以電子郵件傳送"), result.error);
    }
}

void MainWindow::rebuildExternalToolsToolbar() {
    // PRD-UI-016：清單為空時不留一條空工具列佔位；有工具才出現。
    if (externalToolsToolbar_ != nullptr) {
        removeToolBar(externalToolsToolbar_);
        externalToolsToolbar_->deleteLater();
        externalToolsToolbar_ = nullptr;
    }

    const std::vector<app::ExternalTool> tools = settings_->externalTools();
    if (tools.empty()) return;

    externalToolsToolbar_ = addToolBar(tr("外部程式"));
    externalToolsToolbar_->setObjectName(QStringLiteral("externalToolsToolBar"));
    externalToolsToolbar_->setAccessibleName(tr("外部程式工具列"));
    for (const app::ExternalTool& tool : tools) {
        QAction* action = externalToolsToolbar_->addAction(tool.name);
        // 以值捕捉：工具清單隨時可能被管理對話框改動，action 觸發時要用
        // 「按下當下」的設定，不是「工具列建立當下」的設定的參照。
        connect(action, &QAction::triggered, this, [this, tool] { launchExternalTool(tool); });
    }
}

void MainWindow::launchExternalTool(const app::ExternalTool& tool) {
    // 安全限制：設定本身合不合法要先驗證，不合法（例如執行檔被移除、
    // 參數樣板混進不該有的佔位字元）一律擋下，不嘗試「盡量啟動」。
    const app::ExternalToolValidationError error = app::validateExternalTool(tool);
    if (error != app::ExternalToolValidationError::None) {
        QMessageBox::warning(this, tr("啟動外部程式"), app::describeValidationError(error));
        return;
    }

    // {file} 只會展開成目前開啟文件在磁碟上的路徑——不是從 PDF 內容讀出來的
    // 任何欄位，符合「參數不可由 PDF 內容決定」。沒有開啟文件時展開成空字串，
    // 使用者仍能在確認對話框裡看到（並可以取消）。
    const app::ExternalToolLaunchRequest request = app::buildLaunchRequest(tool, currentPath_);

    // 安全限制：啟動前必須顯示完整命令列供使用者確認。
    if (!confirmExternalToolLaunch(tool, request, this)) return;

    const platform::ExternalProcessLaunchResult result =
        platform::defaultExternalProcessLauncher()(app::toExternalProcessRequest(request));
    if (!result.ok) {
        QMessageBox::warning(this, tr("啟動外部程式"), result.error);
    }
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    const QStringList files = settings_->recentFiles();
    if (files.isEmpty()) {
        recentMenu_->addAction(tr("（沒有最近開啟的檔案）"))->setEnabled(false);
        return;
    }
    for (const QString& path : files) {
        auto* action = recentMenu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] { openPath(path); });
    }
    recentMenu_->addSeparator();
    auto* clear = recentMenu_->addAction(tr("清除清單"));
    connect(clear, &QAction::triggered, this, [this] { settings_->clearRecentFiles(); });
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    // 只接受單一 PDF。多檔拖放要等多頁籤（PRD-UI-001）接上才有意義。
    const QMimeData* mime = event->mimeData();
    if (!mime->hasUrls()) return;
    const QList<QUrl> urls = mime->urls();
    if (urls.size() != 1 || !urls.front().isLocalFile()) return;
    if (!urls.front().toLocalFile().endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) return;
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;
    openPath(urls.front().toLocalFile());
    event->acceptProposedAction();
}

void MainWindow::openPath(const QString& path) {
    const QString normalized = app::normalizeDocumentPath(path);
    const auto existing = tabs_.locate(normalized);
    if (existing.has_value()) {
        // 已經開著的文件不再開一次。同一份檔案出現兩個頁籤，兩邊各自寫入
        // 同一個路徑，「檔案被別的程式改過」的守衛會對自己觸發。
        tabs_.setActiveTab(existing->first, existing->second);
    } else {
        app::TabState tab;
        tab.documentId = normalized;
        tab.title = QFileInfo(normalized).fileName();
        (void)tabs_.openTab(tab);
    }
    syncTabBar();
    loadDocument(normalized);
}

void MainWindow::closeTab(int index) {
    const app::WindowState* window = tabs_.window(tabs_.primaryWindowId());
    if (window == nullptr || index < 0 || index >= static_cast<int>(window->tabs.size())) return;

    const bool closingActive = index == window->activeIndex;
    (void)tabs_.closeTab(tabs_.primaryWindowId(), index);
    syncTabBar();

    const app::WindowState* after = tabs_.window(tabs_.primaryWindowId());
    if (after == nullptr || after->tabs.empty()) {
        // 主視窗關到剩零個頁籤時**不關閉視窗**，進入空狀態——使用者仍然
        // 需要一個地方開下一個檔案（TabLayoutModel 的規則，這裡只是照做）。
        storeReadingPosition();
        currentPath_.clear();
        controller_->closeDocument();
        updateWindowTitle(QString());
        return;
    }
    if (closingActive) {
        loadDocument(after->tabs[static_cast<std::size_t>(after->activeIndex)].documentId);
    }
}

void MainWindow::detachTab(int index) {
    const app::WindowState* window = tabs_.window(tabs_.primaryWindowId());
    if (window == nullptr || index < 0 || index >= static_cast<int>(window->tabs.size())) return;
    if (window->tabs.size() < 2) return;  // 唯一的頁籤拖出去只會產生一個空的主視窗

    const QString path = window->tabs[static_cast<std::size_t>(index)].documentId;

    // 新視窗是完整的 MainWindow：它自己的控制器、自己的 PDFium 執行緒、
    // 自己的面板。共用一個控制器的話，兩個視窗會互相搶著開不同的檔案。
    auto* detached = new MainWindow();
    detached->setAttribute(Qt::WA_DeleteOnClose);
    detached->show();
    detached->openPath(path);

    closeTab(index);
}

void MainWindow::syncTabBar() {
    const app::WindowState* window = tabs_.window(tabs_.primaryWindowId());
    if (window == nullptr) return;

    // 重建期間 setCurrentIndex 會發出 currentChanged，那會再開一次檔。
    syncingTabs_ = true;
    while (documentTabs_->count() > static_cast<int>(window->tabs.size())) {
        documentTabs_->removeTab(documentTabs_->count() - 1);
    }
    for (int i = 0; i < static_cast<int>(window->tabs.size()); ++i) {
        const app::TabState& tab = window->tabs[static_cast<std::size_t>(i)];
        if (i >= documentTabs_->count()) {
            documentTabs_->addTab(tab.title);
        } else {
            documentTabs_->setTabText(i, tab.title);
        }
        // 完整路徑放 tooltip：兩份同名的檔案在頁籤上長得一模一樣。
        documentTabs_->setTabToolTip(i, tab.documentId);
        documentTabs_->setTabData(i, tab.documentId);
    }
    if (window->activeIndex >= 0) documentTabs_->setCurrentIndex(window->activeIndex);
    syncingTabs_ = false;

    documentTabs_->setVisible(!window->tabs.empty());
}

void MainWindow::loadDocument(const QString& path) {
    // 換文件前先把前一份的閱讀位置記下來，否則跳到另一份再回來就回到第一頁。
    storeReadingPosition();
    currentPath_ = path;
    settings_->addRecentFile(path);
    commands_->clear();
    updateUndoActions();
    controller_->openDocument(path);
    selection_->openDocument(path);
}

QString MainWindow::annotationAuthor() const {
    const QString author = settings_->authorName();
    return author.isEmpty() ? qEnvironmentVariable("USERNAME", tr("使用者")) : author;
}

void MainWindow::commitAnnotation(app::AnnotationRequest request, const QString& label) {
    if (currentPath_.isEmpty()) return;
    if (!confirmNoExternalChange(label)) return;
    request.path = currentPath_;

    // 所有文件修改都走命令物件（CLAUDE.md）。復原以截回原長度實作：
    // 物件層通道是純附加的，所以那就是精確的反操作。
    auto state = std::make_shared<app::HighlightResult>();
    app::Command command;
    command.label = label;
    command.redo = [this, request, state] {
        *state = annotations_->addAnnotation(request);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path = request.path, state] {
        QString message;
        const bool ok =
            annotations_->revertAppend(path, state->previousSize, state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("註解"),
                             state->message.isEmpty() ? tr("寫入失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    applyToolPersistence();
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::editAlternateText(int structElementObjectNumber, const QString& currentAltText) {
    if (currentPath_.isEmpty()) return;
    if (structElementObjectNumber == 0) {
        QMessageBox::information(this, tr("替代文字"),
                                 tr("這個元素是直接物件，沒有自己的物件編號，無法單獨改寫。"));
        return;
    }

    bool accepted = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("設定替代文字"),
        tr("簡短描述這個元素給螢幕閱讀器使用者聽（留空代表移除既有的替代文字）："),
        currentAltText, &accepted);
    if (!accepted) return;

    app::SetAlternateTextRequest request;
    request.path = currentPath_;
    request.structElementObjectNumber = structElementObjectNumber;
    request.altText = text;

    // 與 commitAnnotation 相同的命令物件模式（CLAUDE.md）：復原以截回原長度實作。
    auto state = std::make_shared<app::SetAlternateTextResult>();
    app::Command command;
    command.label = tr("設定替代文字");
    command.redo = [this, request, state] {
        *state = accessibilityService_->setAlternateText(request);
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    command.undo = [this, path = request.path, state] {
        QString message;
        const bool ok = accessibilityService_->revertAppend(path, state->previousSize,
                                                            state->boundaryGuard, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, tr("替代文字"),
                             state->message.isEmpty() ? tr("寫入失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    statusBar()->showMessage(state->message, 5000);
}

void MainWindow::applyTextMarkup(domain::TextMarkupKind kind, const QString& label) {
    const app::Selection& current = selection_->selection();
    if (current.isEmpty()) {
        QMessageBox::information(this, label, tr("請先選取文字"));
        return;
    }

    // 跨頁選取要產生**每頁一則**註解。PDF 的註解屬於單一頁面，把跨頁的 quad
    // 全部塞進一則裡，第二頁之後的標記會落在第一頁的座標系上——畫出來是
    // 一堆跑到頁面外的色塊，而不是明顯的失敗。
    if (current.isMultiPage()) {
        int written = 0;
        for (const app::PageSelection& page : current.pages) {
            if (page.quads.empty()) continue;
            domain::TextMarkupGeometry pageGeometry;
            pageGeometry.kind = kind;
            pageGeometry.quads = page.quads;

            domain::Annotation pageAnnotation;
            pageAnnotation.geometry = std::move(pageGeometry);
            pageAnnotation.opacity = kind == domain::TextMarkupKind::Highlight ? 0.4 : 1.0;
            pageAnnotation.color = kind == domain::TextMarkupKind::Highlight
                                       ? domain::ColorRgb{1.0, 0.85, 0.0}
                                       : domain::ColorRgb{0.85, 0.1, 0.1};

            app::AnnotationRequest pageRequest;
            pageRequest.pageIndex = page.pageIndex;
            pageRequest.annotation = app::AnnotationService::stamped(
                std::move(pageAnnotation), annotationAuthor(), page.text);
            // 每一頁各自是一個命令。合成一個命令會讓復原只退掉最後一頁，
            // 而使用者看到的是「復原了但前面幾頁的標記還在」。
            commitAnnotation(std::move(pageRequest),
                             tr("%1（第 %2 頁）").arg(label).arg(page.pageIndex + 1));
            ++written;
        }
        selection_->clearSelection();
        if (written > 1) {
            statusBar()->showMessage(tr("已在 %1 頁上加入標記").arg(written), 5000);
        }
        return;
    }

    domain::TextMarkupGeometry geometry;
    geometry.kind = kind;
    geometry.quads = current.quads;

    domain::Annotation annotation;
    annotation.geometry = std::move(geometry);
    // 螢光筆半透明才看得到底下的字；底線與刪除線是實線，透明反而看不清楚。
    annotation.opacity = kind == domain::TextMarkupKind::Highlight ? 0.4 : 1.0;
    annotation.color = kind == domain::TextMarkupKind::Highlight
                           ? domain::ColorRgb{1.0, 0.85, 0.0}
                           : domain::ColorRgb{0.85, 0.1, 0.1};

    app::AnnotationRequest request;
    request.pageIndex = current.pageIndex;
    request.annotation =
        app::AnnotationService::stamped(std::move(annotation), annotationAuthor(), current.text);

    selection_->clearSelection();
    commitAnnotation(std::move(request), label);
}

void MainWindow::applyShape(int pageIndex, const domain::RectF& pageRect) {
    domain::Annotation annotation;
    annotation.rect = pageRect;
    annotation.color = domain::ColorRgb{0.85, 0.1, 0.1};

    QString label;
    switch (pageView_->tool()) {
        case Tool::Rectangle:
            annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Square};
            label = tr("矩形");
            break;
        case Tool::Ellipse:
            annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Circle};
            label = tr("橢圓");
            break;
        case Tool::Measure: {
            // 量測是一條線加上 /Measure。沒有校正過比例尺就算不出真實長度——
            // 這時**不寫出一個看似合理的假數字**，而是先問使用者這條線代表多長。
            domain::LineGeometry line;
            line.start = domain::PointF{pageRect.left, pageRect.top};
            line.end = domain::PointF{pageRect.right, pageRect.bottom};
            annotation.geometry = line;

            if (!measureScale_.isCalibrated()) {
                bool accepted = false;
                const double real = QInputDialog::getDouble(
                    this, tr("校正比例尺"),
                    tr("剛才拉的這條線在真實世界是多長？（公釐）"), 100.0, 0.01, 1e9, 2,
                    &accepted);
                if (!accepted) return;

                app::CalibrationRequest calibration;
                calibration.pointA = line.start;
                calibration.pointB = line.end;
                calibration.realDistance = real;
                const app::CalibrationOutcome outcome =
                    app::MeasurementService::calibrate(calibration);
                if (!outcome.ok) {
                    QMessageBox::warning(this, tr("校正比例尺"), outcome.message);
                    return;
                }
                measureScale_ = outcome.measure;
                statusBar()->showMessage(
                    tr("比例尺已校正為 %1").arg(QString::fromStdString(measureScale_.ratioLabel)),
                    6000);
                // 校正用的那一條線本身不寫成註解：它是一把尺，不是一則量測。
                return;
            }

            bool ok = false;
            QString message;
            annotation = app::MeasurementService::applyMeasurement(std::move(annotation),
                                                                   measureScale_, &ok, &message);
            if (!ok) {
                QMessageBox::warning(this, tr("量測"), message);
                return;
            }
            label = tr("量測");
            break;
        }
        case Tool::Line:
        case Tool::Arrow: {
            domain::LineGeometry line;
            // 拖曳的起點與終點就是 /L 的兩端。用 rect 的對角線而不是左上右下：
            // 使用者由右下往左上拉時，箭頭要指向他放開的那一端。
            line.start = domain::PointF{pageRect.left, pageRect.top};
            line.end = domain::PointF{pageRect.right, pageRect.bottom};
            if (pageView_->tool() == Tool::Arrow) {
                line.endEnding = domain::LineEnding::OpenArrow;
            }
            annotation.geometry = line;
            label = pageView_->tool() == Tool::Arrow ? tr("箭頭") : tr("直線");
            break;
        }
        case Tool::StickyNote: {
            domain::TextNoteGeometry note;
            annotation.geometry = note;
            annotation.color = domain::ColorRgb{1.0, 0.85, 0.0};
            label = tr("便利貼");
            break;
        }
        case Tool::Caret:
            annotation.geometry = domain::CaretGeometry{};
            label = tr("校正符號");
            break;
        case Tool::Stamp: {
            domain::StampGeometry stamp;
            annotation.geometry = stamp;
            label = tr("圖章");
            break;
        }
        case Tool::TextBox:
        case Tool::Typewriter:
        case Tool::Callout: {
            // 這三種都要先有文字才畫得出來——空的文字框在頁面上是一個
            // 看不出用途的空方格，而使用者接下來要找的是「怎麼在裡面打字」。
            bool accepted = false;
            const QString text = QInputDialog::getMultiLineText(
                this, tr("輸入文字"), tr("內容："), QString(), &accepted);
            if (!accepted || text.trimmed().isEmpty()) return;

            app::FreeTextRequest request;
            request.rect = pageRect;
            request.text = text.toStdString();
            request.textColor = domain::ColorRgb{0.0, 0.0, 0.0};
            request.borderColor = annotation.color;

            app::FreeTextBuildResult built;
            if (pageView_->tool() == Tool::Callout) {
                app::CalloutRequest callout;
                callout.rect = pageRect;
                callout.text = request.text;
                callout.textColor = request.textColor;
                callout.borderColor = request.borderColor;
                // 引線預設指向框的左下外側。使用者之後可以拖動端點；
                // 預設指向框自己內部的話，引線整段埋在框裡看不見。
                callout.target = domain::PointF{pageRect.left - 40.0, pageRect.bottom - 40.0};
                built = app::buildCallout(callout);
            } else if (pageView_->tool() == Tool::Typewriter) {
                built = app::buildTypewriter(request);
            } else {
                built = app::buildTextBox(request);
            }

            if (!built.ok) {
                QMessageBox::warning(this, tr("文字方塊"),
                                     QString::fromStdString(built.diagnostic));
                return;
            }
            annotation = std::move(built.annotation);
            label = pageView_->tool() == Tool::Callout      ? tr("指示框")
                    : pageView_->tool() == Tool::Typewriter ? tr("打字機")
                                                            : tr("文字方塊");
            break;
        }
        default:
            return;
    }

    app::AnnotationRequest request;
    request.pageIndex = pageIndex;
    request.annotation = app::AnnotationService::stamped(std::move(annotation), annotationAuthor());
    commitAnnotation(std::move(request), label);
}

bool MainWindow::confirmNoExternalChange(const QString& operation) {
    // PRD-IO-004。存檔前不比對，行為就是「安靜地覆蓋掉別人的版本」，
    // 而那正是 IL-4 要擋的靜默失敗——使用者不會知道自己毀掉了什麼。
    if (currentPath_.isEmpty() || !fileSnapshot_.valid()) return true;

    const platform::ExternalChangeStatus status =
        platform::checkForExternalChange(currentPath_, fileSnapshot_);
    if (status == platform::ExternalChangeStatus::Unchanged ||
        status == platform::ExternalChangeStatus::Unknown) {
        return true;
    }

    if (status == platform::ExternalChangeStatus::DeletedExternally) {
        QMessageBox::warning(this, operation,
                             tr("這個檔案已經被刪除或移走，無法寫入。\n"
                                "請改用「另存新檔」把目前的內容保留下來。"));
        return false;
    }

    // 三個選項，預設是最不具破壞性的那個。「覆蓋」放在最後且不是預設，
    // 因為它會毀掉別人的版本而且無法復原。
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(operation);
    box.setText(tr("這份文件在開啟之後被其他程式修改過。"));
    box.setInformativeText(
        tr("繼續寫入會覆蓋掉那些變更，而且無法復原。\n"
           "「重新載入」會放棄你尚未寫入的修改，換成磁碟上的版本。"));
    QPushButton* reload = box.addButton(tr("重新載入"), QMessageBox::AcceptRole);
    QPushButton* overwrite = box.addButton(tr("仍要覆蓋"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == reload) {
        openPath(currentPath_);
        return false;
    }
    if (box.clickedButton() == overwrite) {
        // 使用者明確選了覆蓋：更新快照，否則下一次寫入會再問一次同一件事。
        fileSnapshot_ = platform::captureSnapshot(currentPath_);
        return true;
    }
    return false;
}

bool MainWindow::confirmRewrite(const QString& operation) {
    if (!controller_->isOpen()) {
        QMessageBox::information(this, operation, tr("尚未開啟文件"));
        return false;
    }
    if (!confirmNoExternalChange(operation)) return false;

    // 這類操作會改動頁面樹或頁面座標系，沒辦法以純附加表達，因此必然全檔重寫。
    // 對有簽章的文件而言那代表簽章直接失效——不是「簽署後有變更」而是「無效」。
    // 使用者有權在動手前知道這件事。
    QString warning = tr("「%1」會重寫整份檔案。").arg(operation);
    if (controller_->info().hasSignatures) {
        warning +=
            tr("\n\n這份文件含數位簽章，操作後簽章將失效，且無法復原為有效狀態。");
    }
    warning += tr("\n\n要繼續嗎？");

    return QMessageBox::warning(this, operation, warning,
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel) == QMessageBox::Yes;
}

void MainWindow::commitPageOperation(const QString& label,
                                     std::function<app::PageOperationResult()> run) {
    auto state = std::make_shared<app::PageOperationResult>();
    app::Command command;
    command.label = label;
    command.redo = [this, run, state] {
        *state = run();
        if (state->ok) reloadCurrentDocument();
        return state->ok;
    };
    // 全檔重寫不能靠「截回原長度」復原，只能把原始位元組寫回去。
    command.undo = [this, path = currentPath_, state] {
        QString message;
        const bool ok = pageOps_->restore(path, state->previousBytes, &message);
        if (ok) {
            reloadCurrentDocument();
        } else {
            statusBar()->showMessage(tr("復原失敗：%1").arg(message), 8000);
        }
        return ok;
    };

    if (!commands_->push(std::move(command))) {
        QMessageBox::warning(this, label,
                             state->message.isEmpty() ? tr("操作失敗") : state->message);
        return;
    }
    updateUndoActions();
    controller_->setDocumentDirty();
    statusBar()->showMessage(state->message, 6000);
}

void MainWindow::buildOrganizeMenu(QMenu* menu) {
    // PRD-PAGE-001：從另一份 PDF 插入頁面。
    auto* insertFromFileAction = menu->addAction(tr("從檔案插入頁面..."));
    registerRibbonAction(QStringLiteral("page.insertFromFile"), insertFromFileAction);
    registerRibbonAction(QStringLiteral("page.insert"), insertFromFileAction);
    connect(insertFromFileAction, &QAction::triggered, this, [this] {
        if (!confirmRewrite(tr("從檔案插入頁面"))) return;

        const QString source = QFileDialog::getOpenFileName(
            this, tr("選擇要插入的 PDF"), QString(), tr("PDF 檔案 (*.pdf)"));
        if (source.isEmpty()) return;
        if (source == currentPath_) {
            // 插入自己會在重寫過程中同時讀寫同一個檔案。擋下來而不是讓它
            // 產生一份半途而廢的輸出。
            QMessageBox::warning(this, tr("從檔案插入頁面"),
                                 tr("不能插入目前正在編輯的這份文件。"));
            return;
        }

        bool accepted = false;
        // 插入位置以「插到第幾頁之前」表達，並允許等於總頁數＋1 代表附加在最後。
        // 用 0 起算的索引問使用者是介面設計的錯——頁碼在畫面上就是從 1 開始的。
        const int before = QInputDialog::getInt(
            this, tr("從檔案插入頁面"), tr("插入到第幾頁之前？（%1 代表附加在最後）")
                                            .arg(controller_->pageCount() + 1),
            controller_->pageCount() + 1, 1, controller_->pageCount() + 1, 1, &accepted);
        if (!accepted) return;

        commitPageOperation(tr("從檔案插入頁面"), [this, source, before] {
            return pageOps_->insertPagesFrom(currentPath_, source, before - 1, {},
                                             app::RewriteConsent::confirmed());
        });
    });

    // PRD-PAGE-001：從純文字插入頁面。
    auto* insertTextAction = menu->addAction(tr("插入文字頁面..."));
    registerRibbonAction(QStringLiteral("page.insertFromText"), insertTextAction);
    connect(insertTextAction, &QAction::triggered, this, [this] {
        if (!confirmRewrite(tr("插入文字頁面"))) return;

        bool accepted = false;
        const QString text = QInputDialog::getMultiLineText(
            this, tr("插入文字頁面"), tr("要排版成頁面的文字："), QString(), &accepted);
        if (!accepted || text.trimmed().isEmpty()) return;

        const int before = QInputDialog::getInt(
            this, tr("插入文字頁面"),
            tr("插入到第幾頁之前？（%1 代表附加在最後）").arg(controller_->pageCount() + 1),
            controller_->pageCount() + 1, 1, controller_->pageCount() + 1, 1, &accepted);
        if (!accepted) return;

        commitPageOperation(tr("插入文字頁面"), [this, text, before] {
            return pageOps_->insertPagesFromText(currentPath_, text, before - 1,
                                                 app::RewriteConsent::confirmed());
        });
    });

    // PRD-PAGE-013：頁面標籤。走增量儲存而不是全檔重寫——只改 /PageLabels
    // 不需要動頁面樹，因此既有簽章仍然是「有效、簽章後有變更」而不是「無效」。
    auto* labelsAction = menu->addAction(tr("頁面標籤（編號範圍）..."));
    registerRibbonAction(QStringLiteral("page.labels"), labelsAction);
    connect(labelsAction, &QAction::triggered, this, [this] { editPageLabels(); });
    menu->addSeparator();

    auto* mergeAction = menu->addAction(tr("合併頁面（N 頁併一頁）..."));
    registerRibbonAction(QStringLiteral("page.move"), mergeAction);
    connect(mergeAction, &QAction::triggered, this, [this] {
        if (!confirmRewrite(tr("合併頁面"))) return;

        bool accepted = false;
        const int perSheet = QInputDialog::getInt(this, tr("合併頁面"),
                                                  tr("每張要放幾頁？"), 2, 2, 16, 1, &accepted);
        if (!accepted) return;

        const int available = controller_->pageCount();
        std::vector<int> pages;
        for (int i = 0; i < std::min(perSheet, available); ++i) pages.push_back(i);
        if (pages.size() < 2) {
            QMessageBox::information(this, tr("合併頁面"), tr("至少需要兩頁"));
            return;
        }

        const domain::compose::MergeLayout layout = domain::compose::MergeLayout::nUp(perSheet);
        commitPageOperation(tr("合併頁面"), [this, pages, layout] {
            return pageOps_->mergePages(currentPath_, pages, layout,
                                        app::RewriteConsent::confirmed());
        });
    });

    auto* overlayAction = menu->addAction(tr("疊加另一份文件..."));
    connect(overlayAction, &QAction::triggered, this, [this] {
        if (!confirmRewrite(tr("疊加"))) return;
        const QString other = QFileDialog::getOpenFileName(this, tr("選擇要疊上的 PDF"), QString(),
                                                           tr("PDF 文件 (*.pdf)"));
        if (other.isEmpty()) return;

        domain::compose::OverlayOptions options;
        commitPageOperation(tr("疊加"), [this, other, options] {
            return pageOps_->overlay(currentPath_, other, options,
                                     app::RewriteConsent::confirmed());
        });
    });

    auto* replaceAction = menu->addAction(tr("取代頁面..."));
    registerRibbonAction(QStringLiteral("page.replace"), replaceAction);
    connect(replaceAction, &QAction::triggered, this, [this] {
        if (!confirmRewrite(tr("取代頁面"))) return;

        bool accepted = false;
        const int page = QInputDialog::getInt(this, tr("取代頁面"), tr("要取代第幾頁？"),
                                              pageView_->pageIndex() + 1, 1,
                                              std::max(1, controller_->pageCount()), 1, &accepted);
        if (!accepted) return;

        const QString other = QFileDialog::getOpenFileName(this, tr("選擇取代用的 PDF"), QString(),
                                                           tr("PDF 文件 (*.pdf)"));
        if (other.isEmpty()) return;

        commitPageOperation(tr("取代頁面"), [this, other, page] {
            return pageOps_->replacePages(currentPath_, other, page - 1, page - 1,
                                          app::RewriteConsent::confirmed());
        });
    });

    // 合併與分割（PRD-PAGE-002）、裁切至白邊（PRD-PAGE-003）。
    // 前兩者不動任何來源檔，所以不進命令堆疊；裁切會重寫原檔，所以會。
    menu->addSeparator();
    auto* mergeDocsAction = menu->addAction(tr("合併多份文件..."));
    registerRibbonAction(QStringLiteral("document.merge"), mergeDocsAction);
    connect(mergeDocsAction, &QAction::triggered, this, [this] { mergeDocuments(); });

    auto* splitDocAction = menu->addAction(tr("分割文件..."));
    registerRibbonAction(QStringLiteral("document.split"), splitDocAction);
    connect(splitDocAction, &QAction::triggered, this, [this] { splitDocument(); });

    // 頁面尺寸調整（PRD-PAGE-003）。縮放政策必須讓使用者選，不能給預設：
    // 「改紙張大小」的兩種意思差很多，而且從操作本身推斷不出來。
    auto* resizeAction = menu->addAction(tr("調整頁面尺寸..."));
    registerRibbonAction(QStringLiteral("page.resize"), resizeAction);
    connect(resizeAction, &QAction::triggered, this, [this] { resizePagesWithDialog(); });

    auto* cropAction = menu->addAction(tr("裁切至白邊"));
    registerRibbonAction(QStringLiteral("page.crop"), cropAction);
    connect(cropAction, &QAction::triggered, this, [this] {
        if (currentPath_.isEmpty() || !controller_->isOpen()) return;
        if (!confirmRewrite(tr("裁切至白邊"))) return;
        commitPageOperation(tr("裁切至白邊"), [this] {
            return pageOps_->cropToContent(currentPath_, {}, app::RewriteConsent::confirmed());
        });
    });

    // 刪除與擷取頁面（PRD-PAGE-002）。範圍以「1,3,5-8」這種寫法輸入——
    // 逐頁勾選在 500 頁的文件上不可用，而縮圖面板的多選只解決得了小文件。
    menu->addSeparator();
    auto* deletePagesAction = menu->addAction(tr("刪除頁面..."));
    registerRibbonAction(QStringLiteral("page.delete"), deletePagesAction);
    connect(deletePagesAction, &QAction::triggered, this, [this] { deletePagesByRange(); });

    // 複製頁面（PDF-XChange 的 Duplicate Page）。複本插在來源頁的正後方——
    // 那是「複製這一頁」最常見的意圖。
    auto* duplicatePagesAction = menu->addAction(tr("複製頁面..."));
    registerRibbonAction(QStringLiteral("page.duplicate"), duplicatePagesAction);
    connect(duplicatePagesAction, &QAction::triggered, this, [this] { duplicatePagesByRange(); });

    auto* extractPagesAction = menu->addAction(tr("擷取頁面另存..."));
    registerRibbonAction(QStringLiteral("page.extract"), extractPagesAction);
    connect(extractPagesAction, &QAction::triggered, this, [this] { extractPagesByRange(); });

    // 頁首頁尾／浮水印／Bates（PRD-PAGE-004）。三者在使用者眼中是三件事，
    // 在實作上是同一件事——「在每頁的某個位置畫一段支援符號展開的文字」，
    // 所以是一個對話框三種預設，不是三份幾乎一樣的程式碼。
    menu->addSeparator();
    const auto addStamp = [&](const QString& text, StampDialog::Preset preset,
                              const QString& id) {
        auto* action = menu->addAction(text);
        connect(action, &QAction::triggered, this,
                [this, preset, text] { applyStamp(preset, text); });
        registerRibbonAction(id, action);
        return action;
    };
    addStamp(tr("頁首頁尾..."), StampDialog::Preset::HeaderFooter,
             QStringLiteral("page.headerFooter"));
    addStamp(tr("浮水印..."), StampDialog::Preset::Watermark, QStringLiteral("page.watermark"));
    addStamp(tr("Bates 編號..."), StampDialog::Preset::Bates, QStringLiteral("page.bates"));

    // 移除所有頁面標記（PDF-XChange 的 Remove All）。只移除本程式加的——
    // 判定依據是寫入時放進串流字典的私有標記鍵。別人加的浮水印移不掉，
    // 那是正確的：移除它需要剖析內容串流並猜哪一段是浮水印。
    auto* removeStampsAction = menu->addAction(tr("移除所有頁面標記"));
    registerRibbonAction(QStringLiteral("page.removeStamps"), removeStampsAction);
    connect(removeStampsAction, &QAction::triggered, this, [this] {
        if (currentPath_.isEmpty() || !controller_->isOpen()) {
            QMessageBox::information(this, tr("移除所有頁面標記"), tr("尚未開啟文件"));
            return;
        }
        if (!confirmNoExternalChange(tr("移除所有頁面標記"))) return;
        const app::StampResult result = stamps_->removeStamps(currentPath_);
        if (!result.ok) {
            QMessageBox::information(this, tr("移除所有頁面標記"), result.message);
            return;
        }
        reloadCurrentDocument();
        statusBar()->showMessage(result.message, 6000);
    });

    // 旋轉頁面。與「向右旋轉檢視」是兩件事：這裡改的是檔案裡的 /Rotate，
    // 存檔後別人打開也是轉過的；檢視旋轉只影響這個視窗的顯示。
    // 兩者共存是刻意的——掃描歪了要改檔案，臨時看橫式表格只要轉畫面。
    menu->addSeparator();
    const auto addRotate = [&](const QString& text, domain::pages::PageRotation rotation,
                               const QString& id) {
        auto* action = menu->addAction(text);
        connect(action, &QAction::triggered, this, [this, rotation, text] {
            if (currentPath_.isEmpty() || !controller_->isOpen()) return;
            if (!confirmRewrite(text)) return;
            std::vector<int> pages;
            pages.reserve(static_cast<std::size_t>(controller_->pageCount()));
            for (int i = 0; i < controller_->pageCount(); ++i) pages.push_back(i);
            commitPageOperation(text, [this, pages, rotation] {
                return pageOps_->rotatePages(currentPath_, pages, rotation,
                                             app::RewriteConsent::confirmed());
            });
        });
        registerRibbonAction(id, action);
        return action;
    };
    addRotate(tr("向右旋轉所有頁面"), domain::pages::PageRotation::Clockwise90,
              QStringLiteral("page.rotate"));
    addRotate(tr("向左旋轉所有頁面"), domain::pages::PageRotation::CounterClockwise90,
              QStringLiteral("page.rotateLeft"));
    addRotate(tr("旋轉所有頁面 180 度"), domain::pages::PageRotation::Half,
              QStringLiteral("page.rotate180"));

    menu->addSeparator();
    auto* normalizeAction = menu->addAction(tr("正規化頁面原點"));
    connect(normalizeAction, &QAction::triggered, this, [this] {
        if (!confirmRewrite(tr("正規化頁面"))) return;
        commitPageOperation(tr("正規化頁面"), [this] {
            return pageOps_->normalize(currentPath_, app::RewriteConsent::confirmed());
        });
    });
}

void MainWindow::restoreSession() {
    const app::Session session = app::loadSession();
    if (!session.windowGeometry.isEmpty()) restoreGeometry(session.windowGeometry);
    // 版本號一升，Qt 就拒絕還原舊的 windowState，改用 applyDefaultPanelLayout()
    // 排好的預設。這是刻意的一次性丟棄：舊版本存下來的是「十三個面板全部展開」，
    // 那不是使用者選的版面，是預設值的缺陷，留著等於讓每個既有使用者
    // 永遠停在壞掉的版面上。之後使用者自己排的版面照常沿用。
    if (!session.windowState.isEmpty()) restoreState(session.windowState, kWindowStateVersion);
    if (session.isEmpty()) return;

    const app::DocumentSession& document =
        session.documents[static_cast<std::size_t>(session.activeIndex)];

    // 開檔是非同步的，所以頁碼與縮放必須等文件真的開好才套用——
    // 立刻套用會被開檔完成時的 fitWidth 蓋掉，使用者看到的是「沒有還原」。
    auto restored = std::make_shared<bool>(false);
    connect(controller_, &app::DocumentController::documentOpened, this,
            [this, document, restored](const QString&) {
                if (*restored) return;
                *restored = true;

                domain::LayoutOptions options = pageView_->layoutOptions();
                options.mode = document.layoutMode;
                options.coverPageSeparate = document.coverPageSeparate;
                options.rightToLeft = document.rightToLeft;
                pageView_->setLayoutOptions(options);

                pageView_->setScale(document.scale);
                pageView_->setPageIndex(document.pageIndex);
                statusBar()->showMessage(tr("已還原上次的閱讀位置"), 4000);
            });

    // 先把所有頁籤放回去，最後才開作用中的那一份。
    //
    // 逐一 openPath 也能建出頁籤，但那會依序真的開啟每一份文件——十個頁籤
    // 就是十次開檔，啟動時間直接爆掉。這裡只登記，開檔留給最後一次。
    for (int i = 0; i < static_cast<int>(session.documents.size()); ++i) {
        app::TabState tab;
        tab.documentId = app::normalizeDocumentPath(session.documents[static_cast<std::size_t>(i)].path);
        tab.title = QFileInfo(tab.documentId).fileName();
        (void)tabs_.openTab(tab);
    }
    syncTabBar();

    openPath(document.path);
}

void MainWindow::storeSession() {
    app::Session session;
    session.windowGeometry = saveGeometry();
    session.windowState = saveState(kWindowStateVersion);

    // 所有開著的頁籤都要記下來，不只作用中的那一個——下次啟動只還原一個頁籤，
    // 使用者會以為其他文件被關掉了。
    //
    // 檢視狀態（頁碼、縮放、版面）只有作用中的那一份是即時的；其餘頁籤的
    // 閱讀位置由 HistoryStore 負責（切換頁籤時就已經記下），這裡只記路徑。
    const app::WindowState* window = tabs_.window(tabs_.primaryWindowId());
    if (window != nullptr) {
        for (int i = 0; i < static_cast<int>(window->tabs.size()); ++i) {
            app::DocumentSession document;
            document.path = window->tabs[static_cast<std::size_t>(i)].documentId;
            if (i == window->activeIndex && controller_->isOpen()) {
                document.pageIndex = pageView_->pageIndex();
                document.scale = pageView_->scale();
                const domain::LayoutOptions& options = pageView_->layoutOptions();
                document.layoutMode = options.mode;
                document.coverPageSeparate = options.coverPageSeparate;
                document.rightToLeft = options.rightToLeft;
                session.activeIndex = i;
            }
            session.documents.push_back(std::move(document));
        }
    }

    app::saveSession(session);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    storeReadingPosition();
    storeSession();
    QMainWindow::closeEvent(event);
}

void MainWindow::followLink(const domain::LinkTarget& target) {
    if (target.isExternal()) {
        // PRD §8.2：開啟網址前必須確認。PDF 是不可信任輸入，一個看起來像頁碼的
        // 連結可以指向任何地方，而使用者點下去之前沒有機會看到目的地。
        const QString uri = QString::fromStdString(target.uri);
        const auto answer = QMessageBox::question(
            this, tr("開啟外部連結"),
            tr("這份文件要求開啟以下網址：\n\n%1\n\n要繼續嗎？").arg(uri),
            QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Open) return;

        // 只放行 http/https。PDF 裡的 URI 可以是 file:// 或其他協定，
        // 交給系統開啟等於讓文件決定要啟動什麼程式。
        const QUrl url(uri);
        const QString scheme = url.scheme().toLower();
        if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
            QMessageBox::warning(this, tr("開啟外部連結"),
                                 tr("只允許 http 與 https 連結，這一個是 %1").arg(scheme));
            return;
        }
        QDesktopServices::openUrl(url);
        return;
    }

    if (target.pageIndex) {
        navigateToPage(*target.pageIndex);
    }
}

void MainWindow::applyHighlight() {
    applyTextMarkup(domain::TextMarkupKind::Highlight, tr("螢光筆"));
}

void MainWindow::reloadCurrentDocument() {
    if (currentPath_.isEmpty()) return;
    // 檔案內容已經變了，重新載入才看得到結果。
    selection_->clearSelection();
    controller_->openDocument(currentPath_);
    selection_->openDocument(currentPath_);
}

void MainWindow::updateUndoActions() {
    undoAction_->setEnabled(commands_->canUndo());
    redoAction_->setEnabled(commands_->canRedo());
    undoAction_->setText(commands_->canUndo() ? tr("復原 %1").arg(commands_->undoLabel())
                                              : tr("復原"));
    redoAction_->setText(commands_->canRedo() ? tr("重做 %1").arg(commands_->redoLabel())
                                              : tr("重做"));
}

void MainWindow::updateWindowTitle(const QString& path) {
    if (path.isEmpty()) {
        setWindowTitle(tr("Alioth PDF 審閱工作站"));
        return;
    }
    setWindowTitle(tr("%1 — Alioth").arg(QFileInfo(path).fileName()));
}

}  // namespace alioth::ui
