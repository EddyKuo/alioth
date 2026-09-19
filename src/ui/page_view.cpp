#include "ui/page_view.h"

#include "domain/zoom_aids.h"

#include <QAccessible>
#include <QAccessibleEvent>
#include <QAccessibleWidget>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPen>
#include <QPolygonF>
#include <QApplication>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <vector>
#include <cmath>

// PRD-UI-013 觸控最佳化：長按判定的時間門檻。輪詢頻率要比門檻本身細得多，
// 否則觸發時間會有一整個 tick 的誤差；50 毫秒對 500 毫秒的門檻是可接受的粒度。
namespace {
constexpr int kLongPressPollMs = 50;
}

namespace alioth::ui {
namespace {

// 貼齊容差。單位是點而不是像素：與命中容差同一個理由——使用者對「夠近了」
// 的感覺不隨縮放改變，用像素會讓放大後幾乎貼不到、縮小後亂貼。
constexpr double kSnapTolerancePt = 6.0;

}  // namespace

namespace {

// PRD-ZOOM-003：8% – 6400%。
constexpr double kMinScale = 0.08;
constexpr double kMaxScale = 64.0;
constexpr double kZoomStep = 1.25;

// PRD §4.3：縮放停止 80 毫秒後才以新倍率重算，避免連續縮放期間每一格都排一輪渲染。
constexpr int kZoomSettleMs = 80;

// 透明度格線（PRD-VIEW-018）的棋盤格尺寸。8 裝置像素是業界慣例
// （Photoshop／Acrobat 皆同），格子太大會被誤認成內容本身的方塊圖案，
// 太小則在高 DPI 螢幕上糊成一片灰。
constexpr int kTransparencyGridCell = 8;

// 畫棋盤格底色。與頁面白底互斥——只在透明度格線模式開啟時呼叫，
// 這時圖磚是帶著真正 alpha 通道渲染出來的（見 engine::RenderOptions::
// transparencyGrid），棋盤格會從圖磚裡真正透明的區域透出來。
void paintTransparencyCheckerboard(QPainter& painter, const QRect& rect) {
    painter.save();
    painter.setClipRect(rect);
    const QColor light(0xFF, 0xFF, 0xFF);
    const QColor dark(0xCC, 0xCC, 0xCC);
    for (int y = rect.top(); y < rect.bottom(); y += kTransparencyGridCell) {
        for (int x = rect.left(); x < rect.right(); x += kTransparencyGridCell) {
            const bool isDark = ((x - rect.left()) / kTransparencyGridCell +
                                 (y - rect.top()) / kTransparencyGridCell) %
                                     2 !=
                                 0;
            painter.fillRect(QRect(x, y, kTransparencyGridCell, kTransparencyGridCell),
                             isDark ? dark : light);
        }
    }
    painter.restore();
}

// 檢視區的 QAccessible 角色（PRD-A11Y-005）。
//
// 不做這件事的話，Qt 會把 QAbstractScrollArea 的子類別回報成
// QAccessible::Client——UIA 看到的是一塊沒有語意的泛用容器，NVDA 會直接
// 略過它，使用者按 PageDown 翻頁後得不到任何回饋。
//
// 角色選 Document 而不是 Canvas 或 Graphic：這裡呈現的是有頁碼、有結構、
// 可導覽的文件，不是一張圖。角色決定螢幕閱讀器提供哪些瀏覽命令，
// 報成圖片會讓使用者失去所有以文件為單位的導覽方式。
class PageViewAccessible : public QAccessibleWidget {
public:
    explicit PageViewAccessible(QWidget* view)
        : QAccessibleWidget(view, QAccessible::Document) {}

    QString text(QAccessible::Text type) const override {
        const auto* view = qobject_cast<const PageView*>(object());
        if (view == nullptr) return QAccessibleWidget::text(type);
        switch (type) {
            case QAccessible::Name:
                return view->accessibleName().isEmpty() ? PageView::tr("PDF 頁面檢視")
                                                        : view->accessibleName();
            case QAccessible::Value:
            case QAccessible::Description:
                // 頁碼與縮放同時放在 Value 與 Description：不同的螢幕閱讀器
                // 讀不同的欄位，只填一個會在另一款上完全沉默。
                return view->accessibleStatusText();
            default:
                return QAccessibleWidget::text(type);
        }
    }
};

QAccessibleInterface* pageViewAccessibleFactory(const QString& className, QObject* object) {
    if (className == QLatin1String("alioth::ui::PageView") && object != nullptr &&
        object->isWidgetType()) {
        return new PageViewAccessible(static_cast<QWidget*>(object));
    }
    return nullptr;
}

}  // namespace

PageView::PageView(app::DocumentController* controller, app::SelectionController* selection,
                   QWidget* parent)
    : QAbstractScrollArea(parent), controller_(controller), selection_(selection) {
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    // PRD-UI-013：沒有這個屬性，Qt 只會把觸控合成成滑鼠事件，長按與雙指縮放
    // 永遠收不到真正的 QTouchEvent。
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);
    touchClock_.start();

    // PRD-A11Y-005：檢視區必須有身分與狀態，否則 UIA 只看到一塊匿名畫布。
    //
    // 工廠只註冊一次。Qt 允許重複安裝同一個函式指標，但那會讓每次查詢都
    // 多走一輪鏈結，而分割檢視會建立第二個 PageView。
    static const bool accessibleFactoryInstalled = [] {
        QAccessible::installFactory(&pageViewAccessibleFactory);
        return true;
    }();
    (void)accessibleFactoryInstalled;

    setObjectName(QStringLiteral("pageView"));
    setAccessibleName(tr("PDF 頁面檢視"));
    announceState();

    connect(selection_, &app::SelectionController::selectionChanged, this,
            [this] { viewport()->update(); });
    viewport()->setAutoFillBackground(true);

    zoomSettleTimer_ = new QTimer(this);
    zoomSettleTimer_->setSingleShot(true);
    zoomSettleTimer_->setInterval(kZoomSettleMs);
    connect(zoomSettleTimer_, &QTimer::timeout, this, [this] {
        zooming_ = false;
        publishViewport();
        viewport()->update();
    });

    longPressTimer_ = new QTimer(this);
    longPressTimer_->setInterval(kLongPressPollMs);
    connect(longPressTimer_, &QTimer::timeout, this, [this] {
        if (!longPress_.checkTimeout(touchClock_.elapsed())) return;
        longPressTimer_->stop();
        const PageHit hit = hitTest(longPress_.startPosition().toPoint());
        if (hit.pageIndex < 0) return;
        emit longPressTriggered(hit.pageIndex, hit.pagePoint, longPress_.startPosition().toPoint());
    });

    connect(controller_, &app::DocumentController::tileReady, this,
            [this](domain::TileKey) { viewport()->update(); });
    // 滑鼠追蹤要開著，否則沒有按鍵時收不到移動事件，游標提示就不會出現。
    viewport()->setMouseTracking(true);

    connect(controller_, &app::DocumentController::linksReady, this,
            [this](int) { viewport()->update(); });

    connect(controller_, &app::DocumentController::pageGeometryChanged, this, [this] {
        syncLayoutSizes();
        publishViewport();
        viewport()->update();
    });
    connect(controller_, &app::DocumentController::documentOpened, this, [this](const QString&) {
        pageIndex_ = 0;
        verticalScrollBar()->setValue(0);
        fitWidth();
        // 總頁數在開檔後才知道，狀態字串必須重算：開檔前是「未開啟文件」。
        announceState();
    });
    connect(controller_, &app::DocumentController::documentReloaded, this, [this](const QString&) {
        // 重載**不可以**動檢視位置。
        //
        // 每一次寫入都會重載（增量儲存架構下這是唯一看得到結果的方式），
        // 而使用者還站在他剛才加註解的那一頁。這裡只重算版面與捲軸範圍：
        // 頁碼、倍率、捲動位置全部原封不動，只有在頁數變少而目前頁超出範圍時
        // 才夾回最後一頁。
        syncLayoutSizes();
        const std::int32_t pages = controller_->pageCount();
        if (pages > 0 && pageIndex_ >= pages) {
            pageIndex_ = pages - 1;
            emit pageChanged(pageIndex_);
        }
        publishViewport();
        announceState();
        viewport()->update();
    });
    connect(controller_, &app::DocumentController::documentClosed, this, [this] {
        // 關檔後版面要跟著空掉。少了這一步，捲軸範圍與頁面矩形都還是上一份
        // 文件的：畫面留著最後那一頁的圖磚，捲動還能捲，而文件其實已經關了。
        pageIndex_ = 0;
        syncLayoutSizes();
        updateScrollRanges();
        horizontalScrollBar()->setValue(0);
        verticalScrollBar()->setValue(0);
        publishViewport();
        announceState();
        viewport()->update();
    });
}

domain::RectI PageView::visibleRectInCurrentPage() const {
    const domain::RectI visible = visibleDocumentRect();
    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        if (placement.pageIndex != pageIndex_) continue;
        const domain::RectI& rect = placement.rect;
        const std::int32_t left = std::max(0, visible.x - rect.x);
        const std::int32_t top = std::max(0, visible.y - rect.y);
        const std::int32_t right = std::min(rect.width, visible.right() - rect.x);
        const std::int32_t bottom = std::min(rect.height, visible.bottom() - rect.y);
        if (right <= left || bottom <= top) return domain::RectI{};
        return domain::RectI{left, top, right - left, bottom - top};
    }
    return domain::RectI{};
}

void PageView::scrollToPointInCurrentPage(const domain::PointF& originInPage) {
    const domain::RectI visible = visibleDocumentRect();
    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        if (placement.pageIndex != pageIndex_) continue;
        horizontalScrollBar()->setValue(
            placement.rect.x + static_cast<int>(std::lround(originInPage.x)));
        verticalScrollBar()->setValue(
            placement.rect.y + static_cast<int>(std::lround(originInPage.y)));
        return;
    }
}

domain::RectI PageView::visibleDocumentRect() const {
    return domain::RectI{horizontalScrollBar()->value(), verticalScrollBar()->value(),
                         viewport()->width(), viewport()->height()};
}

// 版面需要每一頁的尺寸，但尺寸是非同步問到的。尚未問到的頁面由控制器先給 A4 佔位，
// 否則版面會在載入過程中把後面的頁面全部疊在同一個位置。
void PageView::syncLayoutSizes() {
    std::vector<domain::SizeF> sizes;
    const std::int32_t pages = controller_->pageCount();
    sizes.reserve(static_cast<std::size_t>(std::max(0, pages)));
    for (std::int32_t i = 0; i < pages; ++i) {
        sizes.push_back(controller_->pageSizePt(i));
    }
    layout_.setPageSizes(std::move(sizes));
}

void PageView::updateScrollRanges() {
    const domain::SizeF content = layout_.contentSize();
    const int maxX = std::max(0, static_cast<int>(std::ceil(content.width)) - viewport()->width());
    const int maxY = std::max(0, static_cast<int>(std::ceil(content.height)) - viewport()->height());
    horizontalScrollBar()->setRange(0, maxX);
    verticalScrollBar()->setRange(0, maxY);
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setPageStep(viewport()->height());
    horizontalScrollBar()->setSingleStep(48);
    verticalScrollBar()->setSingleStep(48);
}

double PageView::renderScale() const {
    // 圖磚要以實體像素渲染，否則在 150% 或 200% 的螢幕上文字邊緣會糊——
    // 那正是 PRD-VIEW-006 要求的「跨 DPI 拖曳時圖磚重算，文字無模糊」。
    //
    // 版面與捲軸仍然用邏輯像素：那是使用者感知的尺寸，也是 Qt 事件座標的單位。
    // 兩者混用是高 DPI 最常見的錯誤，症狀是內容只畫在畫面的左上四分之一。
    return scale_ * viewport()->devicePixelRatioF();
}

void PageView::publishViewport() {
    layout_.setCurrentPage(pageIndex_);
    layout_.update(scale_, rotation_, layoutOptions_, viewport()->width());
    updateScrollRanges();

    emit viewportChanged();

    const domain::RectI visible = visibleDocumentRect();
    std::vector<app::PageTileRequest> requests;
    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        const domain::RectI& rect = placement.rect;

        // 把可視區換算成頁內座標，超出頁面的部分裁掉——否則會排出頁面外的圖磚，
        // 那些圖磚渲染出來是全白的，白白吃掉渲染預算與快取。
        const std::int32_t left = std::max(0, visible.x - rect.x);
        const std::int32_t top = std::max(0, visible.y - rect.y);
        const std::int32_t right = std::min(rect.width, visible.right() - rect.x);
        const std::int32_t bottom = std::min(rect.height, visible.bottom() - rect.y);
        if (right <= left || bottom <= top) continue;

        // 圖磚座標在渲染像素空間，版面在邏輯像素空間，中間差一個 DPI 係數。
        const double dpr = viewport()->devicePixelRatioF();
        const auto toRender = [dpr](std::int32_t value) {
            return static_cast<std::int32_t>(std::lround(value * dpr));
        };

        app::PageTileRequest request;
        request.pageIndex = placement.pageIndex;
        request.visibleInPage = domain::RectI{toRender(left), toRender(top),
                                              toRender(right - left), toRender(bottom - top)};
        request.pageSize = domain::RectI{0, 0, toRender(rect.width), toRender(rect.height)};
        requests.push_back(request);
    }

    for (const app::PageTileRequest& request : requests) {
        controller_->requestLinks(request.pageIndex);
    }

    app::TileScheduleOptions options;
    options.scale = renderScale();
    options.rotation = rotation_;
    options.zooming = zooming_;
    controller_->scheduleTiles(requests, options);
}

void PageView::setLayoutOptions(const domain::LayoutOptions& options) {
    layoutOptions_ = options;
    publishViewport();
    scrollToPage(pageIndex_);
    viewport()->update();
}

void PageView::setPageIndex(std::int32_t index) {
    if (index < 0 || index >= controller_->pageCount()) return;
    if (pageIndex_ == index) return;
    pageIndex_ = index;
    publishViewport();
    scrollToPage(index);
    emit pageChanged(pageIndex_);
    announceState();
    viewport()->update();
}

QString PageView::accessibleStatusText() const {
    const int total = controller_->pageCount();
    if (total <= 0) return tr("未開啟文件");
    // 縮放以整數百分比報出。念「一二五點零零百分比」對使用者沒有幫助，
    // 而小數點在語音合成裡特別容易聽錯。
    const int percent = static_cast<int>(std::lround(scale_ * 100.0));
    return tr("第 %1 頁，共 %2 頁，縮放 %3%").arg(pageIndex_ + 1).arg(total).arg(percent);
}

void PageView::announceState() {
    const QString status = accessibleStatusText();
    if (accessibleDescription() == status) return;
    setAccessibleDescription(status);

    // 只改字串不發事件等於沒改：NVDA 與 JAWS 會快取描述，不會主動重讀。
    // 用 DescriptionChanged 而不是 ValueChanged，因為這個元件沒有數值語意，
    // 回報成數值會讓螢幕閱讀器念出「滑桿」之類的錯誤角色提示。
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}

bool PageView::scrollByPixels(int delta) {
    if (delta == 0) return true;
    QScrollBar* bar = verticalScrollBar();
    const int before = bar->value();
    bar->setValue(before + delta);
    // 值沒動代表已經在端點。捲動條會自己夾住，所以只能靠前後比較判斷。
    return bar->value() != before;
}

void PageView::scrollToPage(std::int32_t index) {
    const domain::RectI rect = layout_.pageRect(index);
    if (rect.isEmpty()) return;
    verticalScrollBar()->setValue(rect.y);
    publishViewport();
}

void PageView::zoomToPageRect(std::int32_t pageIndex, const domain::RectF& pageRect) {
    const domain::RectI placement = layout_.pageRect(pageIndex);
    if (placement.isEmpty()) return;

    // 頁面座標（Y 向上）→ 頁內裝置座標（邏輯像素、目前倍率）。
    // 這裡刻意用 scale_ 而不是 renderScale()：框選與捲軸都在邏輯像素空間，
    // 混進裝置像素比會讓高 DPI 螢幕上縮放到錯誤的倍率。
    const domain::PageTransform transform(controller_->pageSizePt(pageIndex), scale_, rotation_);
    const domain::RectF box = pageRect.normalized();
    const domain::PointF a = transform.toDevice(domain::PointF{box.left, box.top});
    const domain::PointF b = transform.toDevice(domain::PointF{box.right, box.bottom});

    domain::RectZoomRequest request;
    request.selection = domain::RectI{
        static_cast<std::int32_t>(std::lround(placement.x + std::min(a.x, b.x))),
        static_cast<std::int32_t>(std::lround(placement.y + std::min(a.y, b.y))),
        static_cast<std::int32_t>(std::lround(std::abs(b.x - a.x))),
        static_cast<std::int32_t>(std::lround(std::abs(b.y - a.y)))};
    request.currentScale = scale_;
    request.viewportPx = domain::SizeF{static_cast<double>(viewport()->width()),
                                       static_cast<double>(viewport()->height())};
    request.minScale = kMinScale;
    request.maxScale = kMaxScale;

    const domain::RectZoomResult result = domain::computeRectZoom(request);
    // 框選退化（單點點擊）時 computeRectZoom 回傳目前倍率，不要白做一次縮放。
    if (std::abs(result.newScale - scale_) < 1e-9) return;

    setScale(result.newScale);
    // setScale 以可視區中心為錨，這裡再把框選中心推到中心——兩步是刻意的，
    // 中間那一步讓倍率與版面先安定下來，捲軸範圍才是新倍率下的範圍。
    horizontalScrollBar()->setValue(static_cast<int>(
        std::lround(result.centerAtNewScale.x - viewport()->width() / 2.0)));
    verticalScrollBar()->setValue(static_cast<int>(
        std::lround(result.centerAtNewScale.y - viewport()->height() / 2.0)));
    publishViewport();
    viewport()->update();
}

void PageView::setScale(double scale) {
    applyZoomAnchored(scale, QPointF(viewport()->rect().center()));
}

void PageView::applyZoomAnchored(double newScale, const QPointF& anchorInWidget) {
    const double clamped = std::clamp(newScale, kMinScale, kMaxScale);
    if (std::abs(clamped - scale_) < 1e-9) return;

    // 先記下錨點在「未縮放的文件空間」中的位置，縮放後把捲軸推回去，
    // 使該點停在畫面上同一個位置。PRD-ZOOM-002 要求連續縮放 10 次偏移 ≤ 2 像素。
    const double docX = (horizontalScrollBar()->value() + anchorInWidget.x()) / scale_;
    const double docY = (verticalScrollBar()->value() + anchorInWidget.y()) / scale_;

    scale_ = clamped;
    zooming_ = true;
    zoomSettleTimer_->start();

    // 版面必須先以新倍率重算，捲軸範圍才是對的，否則 setValue 會被舊範圍夾掉。
    publishViewport();

    horizontalScrollBar()->setValue(
        static_cast<int>(std::lround(docX * scale_ - anchorInWidget.x())));
    verticalScrollBar()->setValue(
        static_cast<int>(std::lround(docY * scale_ - anchorInWidget.y())));

    publishViewport();
    emit scaleChanged(scale_);
    announceState();
    viewport()->update();
}

void PageView::zoomIn() { applyZoomAnchored(scale_ * kZoomStep, QPointF(viewport()->rect().center())); }
void PageView::zoomOut() { applyZoomAnchored(scale_ / kZoomStep, QPointF(viewport()->rect().center())); }

void PageView::actualSize() { setScale(1.0); }

void PageView::fitPage() {
    const domain::SizeF pageSize = controller_->pageSizePt(pageIndex_);
    if (pageSize.isEmpty()) return;
    const bool swap = domain::swapsAxes(rotation_);
    const double w = swap ? pageSize.height : pageSize.width;
    const double h = swap ? pageSize.width : pageSize.height;
    const double margin = layoutOptions_.pageGapPx * 2.0;
    setScale(std::min((viewport()->width() - margin) / w, (viewport()->height() - margin) / h));
}

void PageView::fitWidth() {
    const domain::SizeF pageSize = controller_->pageSizePt(pageIndex_);
    if (pageSize.isEmpty()) return;
    const double w = domain::swapsAxes(rotation_) ? pageSize.height : pageSize.width;
    setScale((viewport()->width() - layoutOptions_.pageGapPx * 2.0) / w);
}

void PageView::setRotation(domain::Rotation rotation) {
    if (rotation_ == rotation) return;
    rotation_ = rotation;
    publishViewport();
    scrollToPage(pageIndex_);
    viewport()->update();
}

void PageView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    publishViewport();
}

void PageView::scrollContentsBy(int dx, int dy) {
    QAbstractScrollArea::scrollContentsBy(dx, dy);
    publishViewport();

    // 連續模式下捲動會換頁，狀態列與縮圖選取要跟著走。
    const std::int32_t centred =
        layout_.pageAtViewportCenter(visibleDocumentRect());
    if (centred != pageIndex_) {
        pageIndex_ = centred;
        emit pageChanged(pageIndex_);
        // 捲動換頁也要回報。少了這裡，使用者用捲軸翻頁時螢幕閱讀器全程沉默。
        announceState();
    }
}

void PageView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const double delta = event->angleDelta().y() / 120.0;
        applyZoomAnchored(scale_ * std::pow(kZoomStep, delta), event->position());
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PageView::keyPressEvent(QKeyEvent* event) {
    // 多邊形／折線進行中時，Enter 結束、Esc 取消。
    // 這兩鍵必須優先於翻頁——正在放頂點的人按 Enter 想要的是「畫完」，
    // 不是「跳到下一頁然後把畫到一半的形狀丟掉」。
    if (pendingVertexPage_ >= 0) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishVertexDrawing(true);
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            finishVertexDrawing(false);
            return;
        }
    }
    switch (event->key()) {
        case Qt::Key_PageDown: setPageIndex(pageIndex_ + 1); return;
        case Qt::Key_PageUp:   setPageIndex(pageIndex_ - 1); return;
        case Qt::Key_Home:     setPageIndex(0); return;
        case Qt::Key_End:      setPageIndex(controller_->pageCount() - 1); return;
        default: break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

PageView::PageHit PageView::hitTest(const QPoint& widgetPoint) const {
    const domain::RectI visible = visibleDocumentRect();
    const domain::PointF documentPoint{static_cast<double>(widgetPoint.x() + visible.x),
                                       static_cast<double>(widgetPoint.y() + visible.y)};

    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        const domain::RectI& rect = placement.rect;
        if (documentPoint.x < rect.x || documentPoint.x >= rect.right() ||
            documentPoint.y < rect.y || documentPoint.y >= rect.bottom()) {
            continue;
        }

        // 頁內裝置座標 → 頁面座標。Y 軸翻轉與旋轉一律交給 PageTransform，
        // 這是全專案唯一該做這件事的地方。
        const domain::PageTransform transform(controller_->pageSizePt(placement.pageIndex), scale_,
                                              rotation_);
        const domain::PointF local{documentPoint.x - rect.x, documentPoint.y - rect.y};
        return PageHit{placement.pageIndex, transform.toPage(local)};
    }
    return PageHit{};
}

void PageView::setTool(Tool tool) {
    if (tool_ == tool) return;
    tool_ = tool;
    shapePage_ = -1;
    // 游標形狀是使用者判斷「現在是什麼工具」最快的線索，比工具列上的按下狀態更即時。
    updateToolCursor();
    if (tool_ != Tool::Select) selection_->clearSelection();
    viewport()->update();
}

void PageView::setCursorSizeLevel(app::CursorSizeLevel level) {
    if (cursorSizeLevel_ == level) return;
    cursorSizeLevel_ = level;
    updateToolCursor();
}

void PageView::finishVertexDrawing(bool commit) {
    const std::int32_t page = pendingVertexPage_;
    std::vector<domain::PointF> vertices;
    vertices.swap(pendingVertices_);
    pendingVertexPage_ = -1;
    viewport()->update();

    if (!commit || page < 0) return;
    // 少於兩點的折線與少於三點的多邊形不是形狀，是誤點。
    const std::size_t minimum = tool_ == Tool::PolyLine ? 2u : 3u;
    if (vertices.size() < minimum) return;
    emit verticesDrawn(page, vertices, tool_ != Tool::PolyLine);
}

bool PageView::cancelPendingGesture() {
    if (pendingVertexPage_ >= 0) {
        finishVertexDrawing(false);
        return true;
    }
    if (pendingStrokePage_ >= 0) {
        pendingStroke_.clear();
        pendingStrokePage_ = -1;
        dragging_ = false;
        viewport()->update();
        return true;
    }
    if (shapePage_ >= 0) {
        shapePage_ = -1;
        dragging_ = false;
        viewport()->update();
        return true;
    }
    return false;
}

void PageView::setFormFieldHighlight(bool enabled) {
    if (highlightFields_ == enabled) return;
    highlightFields_ = enabled;
    viewport()->update();
}

void PageView::setFormFieldRects(std::vector<std::pair<int, domain::RectF>> rects) {
    fieldRects_ = std::move(rects);
    if (highlightFields_) viewport()->update();
}

void PageView::setHighlightedRect(int pageIndex, const domain::RectF& pageRect) {
    if (highlightPage_ == pageIndex && highlightRect_ == pageRect) return;
    highlightPage_ = pageIndex;
    highlightRect_ = pageRect;
    viewport()->update();
}

void PageView::clearHighlightedRect() { setHighlightedRect(-1, domain::RectF{}); }

void PageView::paintHighlightedRect(QPainter& painter, const domain::RectI& visible) const {
    if (highlightPage_ < 0) return;

    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        if (placement.pageIndex != highlightPage_) continue;
        const domain::PageTransform transform{controller_->pageSizePt(placement.pageIndex),
                                              renderScale(), rotation_};
        const QPoint origin(placement.rect.x - visible.x, placement.rect.y - visible.y);
        const domain::RectF box = highlightRect_.normalized();
        const domain::PointF topLeft = transform.toDevice(domain::PointF{box.left, box.top});
        const domain::PointF bottomRight =
            transform.toDevice(domain::PointF{box.right, box.bottom});
        QRectF rect(QPointF(origin.x() + topLeft.x, origin.y() + topLeft.y),
                    QPointF(origin.x() + bottomRight.x, origin.y() + bottomRight.y));
        rect = rect.normalized().adjusted(-2.0, -2.0, 2.0, 2.0);

        painter.save();
        // 虛線外框、不填色。填色會蓋掉被選中的那則註解本身，而使用者選它
        // 通常正是為了看清楚它。虛線則讓「這是選取狀態」與「這是文件內容」
        // 不會被混淆——實線方框在工程圖上本來就到處都是。
        QPen pen(QColor(0x1E, 0x90, 0xFF));
        pen.setWidth(2);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
        painter.restore();
        return;
    }
}

void PageView::paintFormFieldHighlights(QPainter& painter, const domain::RectI& visible) const {
    if (!highlightFields_ || fieldRects_.empty()) return;

    painter.save();
    // 半透明填色加實線外框。只畫外框在深色內容上看不見，只填色又會蓋掉
    // 欄位裡已經填好的文字——兩者都是「標示」變成「妨礙」的方式。
    const QColor fill(0x33, 0x66, 0xCC, 40);
    QPen pen(QColor(0x33, 0x66, 0xCC));
    pen.setWidth(1);

    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        const domain::PageTransform transform{controller_->pageSizePt(placement.pageIndex),
                                              renderScale(), rotation_};
        const QPoint origin(placement.rect.x - visible.x, placement.rect.y - visible.y);
        for (const auto& entry : fieldRects_) {
            if (entry.first != placement.pageIndex) continue;
            const domain::RectF box = entry.second.normalized();
            const domain::PointF topLeft = transform.toDevice(domain::PointF{box.left, box.top});
            const domain::PointF bottomRight =
                transform.toDevice(domain::PointF{box.right, box.bottom});
            const QRectF rect(QPointF(origin.x() + topLeft.x, origin.y() + topLeft.y),
                              QPointF(origin.x() + bottomRight.x, origin.y() + bottomRight.y));
            painter.setPen(pen);
            painter.setBrush(fill);
            painter.drawRect(rect.normalized());
        }
    }
    painter.restore();
}

void PageView::paintPendingStroke(QPainter& painter, const domain::RectI& visible) const {
    if (pendingStroke_.size() < 2 || pendingStrokePage_ < 0) return;

    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        if (placement.pageIndex != pendingStrokePage_) continue;
        const domain::PageTransform transform{controller_->pageSizePt(placement.pageIndex),
                                              renderScale(), rotation_};
        const QPoint origin(placement.rect.x - visible.x, placement.rect.y - visible.y);

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        // 逐段畫，讓線寬跟著壓力變化——預覽若一律等寬，使用者要等到寫進檔案
        // 才會第一次看見壓力的效果，那時已經沒辦法調整力道了。
        for (std::size_t i = 1; i < pendingStroke_.size(); ++i) {
            const domain::PointF a = transform.toDevice(pendingStroke_[i - 1].position);
            const domain::PointF b = transform.toDevice(pendingStroke_[i].position);
            const double pressure = std::clamp(pendingStroke_[i].pressure, 0.0, 1.0);
            QPen pen(QColor(217, 26, 26));
            pen.setWidthF(1.0 + pressure * 3.0);
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            painter.drawLine(QPointF(origin.x() + a.x, origin.y() + a.y),
                             QPointF(origin.x() + b.x, origin.y() + b.y));
        }
        painter.restore();
        return;
    }
}

void PageView::paintPendingVertices(QPainter& painter, const domain::RectI& visible) const {
    if (pendingVertices_.empty() || pendingVertexPage_ < 0) return;

    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        if (placement.pageIndex != pendingVertexPage_) continue;
        const domain::PageTransform transform{controller_->pageSizePt(placement.pageIndex),
                                              renderScale(), rotation_};
        const QPoint origin(placement.rect.x - visible.x, placement.rect.y - visible.y);

        QPolygonF path;
        for (const domain::PointF& vertex : pendingVertices_) {
            const domain::PointF device = transform.toDevice(vertex);
            path << QPointF(origin.x() + device.x, origin.y() + device.y);
        }

        painter.save();
        QPen pen(QColor(217, 26, 26));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(path);
        // 頂點畫成小方塊：只有線的話，使用者看不出自己已經放了幾個點，
        // 而點數正是他要決定何時結束的依據。
        for (const QPointF& point : path) {
            painter.drawRect(QRectF(point.x() - 2.0, point.y() - 2.0, 4.0, 4.0));
        }
        painter.restore();
        return;
    }
}

void PageView::setGuidesVisible(bool visible) {
    guidesVisible_ = visible;
    viewport()->update();
}

void PageView::setGrid(const domain::GridSettings& grid) {
    grid_ = grid;
    viewport()->update();
}

void PageView::setSnapEnabled(bool enabled) { snapEnabled_ = enabled; }

domain::PageTransform PageView::currentPageTransform() const {
    return domain::PageTransform{controller_->pageSizePt(pageIndex_), renderScale(), rotation_};
}

QPoint PageView::currentPageOriginInViewport() const {
    const domain::RectI visible = visibleDocumentRect();
    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        if (placement.pageIndex != pageIndex_) continue;
        return QPoint(placement.rect.x - visible.x, placement.rect.y - visible.y);
    }
    return QPoint(0, 0);
}

void PageView::updateToolCursor() {
    if (tool_ == Tool::Select) {
        // IBeam 是系統游標：Windows 的「指標大小」設定會自動縮放它，
        // 應用程式不需要（也不應該）自己重畫。
        viewport()->setCursor(Qt::IBeamCursor);
        return;
    }
    if (tool_ == Tool::Hand) {
        // 張開的手代表「可以抓」。同樣是系統游標，交給系統縮放。
        viewport()->setCursor(Qt::OpenHandCursor);
        return;
    }
    if (tool_ == Tool::SelectComment) {
        // 箭頭：這個工具選的是物件不是文字，也不畫東西。十字準星會讓人
        // 以為可以拖出一個形狀。
        viewport()->setCursor(Qt::ArrowCursor);
        return;
    }
    // 十字準星是自畫游標（PRD-UI-018）：系統的指標大小設定不會幫我們縮放
    // 一張自己畫的點陣圖，必須依使用者選的等級與目前螢幕 DPI 自行重畫。
    viewport()->setCursor(
        app::CursorScale::buildCrosshairCursor(cursorSizeLevel_, viewport()->devicePixelRatioF()));
}

const domain::LinkTarget* PageView::linkAt(const PageHit& hit) const {
    if (hit.pageIndex < 0) return nullptr;
    for (const domain::LinkTarget& link : controller_->linksForPage(hit.pageIndex)) {
        if (link.rect.contains(hit.pagePoint)) return &link;
    }
    return nullptr;
}

void PageView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const PageHit hit = hitTest(event->pos());
    if (hit.pageIndex < 0) {
        selection_->clearSelection();
        return;
    }

    // 三擊選行（PRD-TXT-004）。位移門檻是必要的：少了它，快速雙擊之後
    // 在別處點一下會選到那裡的一整行，而使用者只是想把游標移過去。
    constexpr int kTripleClickSlopPx = 6;
    if (sinceDoubleClick_.isValid() &&
        sinceDoubleClick_.elapsed() < QApplication::doubleClickInterval() &&
        (event->pos() - lastDoubleClickPos_).manhattanLength() <= kTripleClickSlopPx) {
        // 用完就作廢，否則第四下、第五下都會再選一次行。
        sinceDoubleClick_.invalidate();
        selection_->selectLineAt(hit.pageIndex, hit.pagePoint);
        return;
    }
    sinceDoubleClick_.invalidate();

    // 連結優先於選取工具：使用者點在連結上時的意圖幾乎一定是跟隨它。
    if (tool_ == Tool::Select) {
        if (const domain::LinkTarget* link = linkAt(hit)) {
            emit linkActivated(*link);
            return;
        }
        emit pageClicked(hit.pageIndex, hit.pagePoint);
    }

    // 選取註解工具：只回報點了哪裡，不進入拖曳也不碰選取。命中測試在應用層
    // ——PageView 不認識註解，那條規則在這個工具上也不破例。
    if (tool_ == Tool::SelectComment) {
        emit pageClicked(hit.pageIndex, hit.pagePoint);
        return;
    }

    dragging_ = true;

    // 手形：只記起點，實際捲動在 mouseMoveEvent。不碰選取，也不畫東西。
    if (tool_ == Tool::Hand) {
        panAnchor_ = event->pos();
        return;
    }

    // 文字標記類的工具仍然走選取：螢光筆要貼著字，不是畫一個方框。
    if (tool_ == Tool::Select || tool_ == Tool::Highlight || tool_ == Tool::Underline ||
        tool_ == Tool::StrikeOut || tool_ == Tool::Squiggly) {
        selection_->beginSelection(hit.pageIndex, hit.pagePoint);
        return;
    }

    // 多邊形／折線：每一次點擊加一個頂點，不進入拖曳。
    if (tool_ == Tool::Polygon || tool_ == Tool::PolyLine || tool_ == Tool::Cloud) {
        dragging_ = false;
        if (pendingVertexPage_ >= 0 && pendingVertexPage_ != hit.pageIndex) {
            // 換頁就把前一段結束掉。跨頁的多邊形在 PDF 裡不存在——
            // 註解屬於單一頁面，硬做只會產生一個頂點在頁面外的形狀。
            finishVertexDrawing(true);
        }
        pendingVertexPage_ = hit.pageIndex;
        pendingVertices_.push_back(
            snapEnabled_
                ? domain::snapPoint(hit.pagePoint, guides_, grid_, {}, {}, kSnapTolerancePt).point()
                : hit.pagePoint);
        viewport()->update();
        return;
    }

    // 鉛筆：逐點收集，不是拖出一個矩形。刻意不貼齊格線——手繪的意義就是
    // 不受格線約束，貼齊會把一條隨手畫的線折成階梯。
    if (tool_ == Tool::Pencil) {
        pendingStrokePage_ = hit.pageIndex;
        pendingStroke_.clear();
        pendingStroke_.push_back(domain::PressurePoint{hit.pagePoint, currentPressure_});
        return;
    }

    shapePage_ = hit.pageIndex;
    shapeAnchor_ = snapEnabled_ ? domain::snapPoint(hit.pagePoint, guides_, grid_, {}, {},
                                                   kSnapTolerancePt)
                                      .point()
                                : hit.pagePoint;
    shapeCurrent_ = shapeAnchor_;
}

void PageView::mouseMoveEvent(QMouseEvent* event) {
    if (const PageHit hover = hitTest(event->pos()); hover.pageIndex >= 0) {
        emit cursorMoved(hover.pageIndex, hover.pagePoint);
    }

    // 手形拖曳：內容跟著手走，所以捲軸要往**相反**方向移動。
    // 反了的話畫面會朝著與手指相反的方向跑，那是立刻會被察覺的錯。
    if (dragging_ && tool_ == Tool::Hand) {
        const QPoint delta = event->pos() - panAnchor_;
        panAnchor_ = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        return;
    }

    if (!dragging_) {
        // 手形游標是使用者判斷「這裡可以點」的唯一線索——連結在 PDF 上沒有底線。
        if (tool_ == Tool::Select) {
            const bool overLink = linkAt(hitTest(event->pos())) != nullptr;
            viewport()->setCursor(overLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
        }
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    const PageHit hit = hitTest(event->pos());
    if (hit.pageIndex < 0) return;

    if (pendingStrokePage_ >= 0) {
        // 跨頁的筆畫在 PDF 裡不存在（註解屬於單一頁面），移出這一頁就不再收點。
        if (hit.pageIndex == pendingStrokePage_) {
            pendingStroke_.push_back(domain::PressurePoint{hit.pagePoint, currentPressure_});
            viewport()->update();
        }
        return;
    }

    if (shapePage_ >= 0) {
        // 跨頁拖曳沒有明確語意，維持在起始頁。
        if (hit.pageIndex == shapePage_) {
            shapeCurrent_ = snapEnabled_
                                ? domain::snapPoint(hit.pagePoint, guides_, grid_, {}, {},
                                                    kSnapTolerancePt)
                                      .point()
                                : hit.pagePoint;
            viewport()->update();
        }
        return;
    }
    selection_->extendSelection(hit.pageIndex, hit.pagePoint);
}

void PageView::mouseReleaseEvent(QMouseEvent* event) {
    const bool wasDragging = dragging_;
    dragging_ = false;

    // 文字標記工具：放開滑鼠就是「這段標起來」。少了這一步，選了底線工具
    // 拖過一段文字之後什麼都不會發生——工具看起來壞掉，而它其實只是
    // 選了字而已。
    if (wasDragging && (tool_ == Tool::Highlight || tool_ == Tool::Underline ||
                        tool_ == Tool::StrikeOut || tool_ == Tool::Squiggly) &&
        !selection_->selection().isEmpty()) {
        emit markupSelectionCompleted();
    }

    if (pendingStrokePage_ >= 0) {
        const std::int32_t page = pendingStrokePage_;
        std::vector<domain::PressurePoint> stroke;
        stroke.swap(pendingStroke_);
        pendingStrokePage_ = -1;
        viewport()->update();
        // 一個點畫不出線。點一下就放開多半是誤觸，寫出去只會是一則看不見的註解。
        if (stroke.size() >= 2) emit strokeDrawn(page, stroke);
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }

    if (shapePage_ >= 0) {
        const domain::RectF rect =
            domain::RectF{shapeAnchor_.x, shapeAnchor_.y, shapeCurrent_.x, shapeCurrent_.y}
                .normalized();
        const std::int32_t page = shapePage_;
        shapePage_ = -1;
        viewport()->update();

        // 太小的拖曳多半是誤點，不是想畫一個一像素的方框。
        // 便利貼是例外——它本來就是點一下就放。
        constexpr double kMinimumExtentPt = 3.0;
        const bool bigEnough =
            rect.width() >= kMinimumExtentPt && rect.height() >= kMinimumExtentPt;
        if (tool_ == Tool::AreaSelect || tool_ == Tool::Snapshot ||
            tool_ == Tool::RedactMark || tool_ == Tool::FormField ||
            tool_ == Tool::ZoomArea) {
            if (bigEnough) emit areaSelected(page, rect);
        } else if (tool_ == Tool::StickyNote || bigEnough) {
            emit shapeDrawn(page, rect);
        }
    }

    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PageView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (tool_ == Tool::Polygon || tool_ == Tool::PolyLine || tool_ == Tool::Cloud) {
        finishVertexDrawing(true);
        return;
    }
    const PageHit hit = hitTest(event->pos());
    if (hit.pageIndex < 0) return;

    // 選取註解工具的雙擊是「開啟這則註解」，不是選詞。
    if (tool_ == Tool::SelectComment) {
        emit pageDoubleClicked(hit.pageIndex, hit.pagePoint);
        return;
    }

    // 雙擊選詞（PRD-TXT-004）。
    //
    // 三擊選行沒有對應的 Qt 事件——Qt 送的序列是
    // press/release/doubleClick/release/press/release，第三次只是一個普通的 press。
    // 因此在這裡記下雙擊的時間與位置，由 mousePressEvent 判斷緊接著的那一次
    // press 是不是同一個手勢的第三下。
    sinceDoubleClick_.start();
    lastDoubleClickPos_ = event->pos();
    selection_->selectWordAt(hit.pageIndex, hit.pagePoint);
}

void PageView::tabletEvent(QTabletEvent* event) {
    // 只借用壓力值，事件本身仍然交回給 Qt 合成成滑鼠事件。
    //
    // 自己在這裡處理按下／移動／放開會產生第二條輸入路徑，兩條路徑遲早會
    // 在某個狀態上分岔（例如手寫筆按下後改用滑鼠放開）。讓壓力是唯一從
    // 數位板取得的資訊，其餘一律走既有的滑鼠路徑，狀態機就只有一份。
    currentPressure_ = event->pressure() > 0.0 ? event->pressure() : 1.0;
    if (event->type() == QEvent::TabletRelease) {
        // 筆離開之後回到「沒有壓力裝置」的預設值，否則接下來用滑鼠畫的線
        // 會沿用最後一次的筆壓。
        currentPressure_ = 1.0;
    }
    event->ignore();
}

bool PageView::viewportEvent(QEvent* event) {
    switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            if (handleTouchEvent(event)) return true;
            break;
        default:
            break;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

bool PageView::handleTouchEvent(QEvent* event) {
    auto* touchEvent = static_cast<QTouchEvent*>(event);
    const QList<QTouchEvent::TouchPoint> points = touchEvent->points();

    if (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel ||
        points.isEmpty()) {
        longPress_.release();
        longPressTimer_->stop();
        pinchActive_ = false;
        event->accept();
        return true;
    }

    if (points.size() == 1) {
        // 雙指縮放中途放開一指會回到單指——不要把那一刻誤判為新的長按候選，
        // 一律先結束 pinch 狀態再走單指邏輯。
        pinchActive_ = false;
        const QPointF pos = points.first().position();
        const qint64 now = touchClock_.elapsed();
        if (event->type() == QEvent::TouchBegin) {
            longPress_.press(pos, now);
            longPressTimer_->start();
        } else {
            longPress_.move(pos, now);
        }
    } else if (points.size() >= 2) {
        // 兩指以上一律只取前兩個點：多指手勢（三指以上）不在 PRD-UI-013 範圍內，
        // 忽略多出來的觸點比誤判成別的手勢安全。
        longPress_.release();
        longPressTimer_->stop();

        const QPointF p1 = points[0].position();
        const QPointF p2 = points[1].position();
        if (!pinchActive_) {
            pinchTracker_.begin(p1, p2, scale_);
            pinchActive_ = true;
        } else {
            const app::PinchZoomTracker::Update update = pinchTracker_.update(p1, p2);
            applyZoomAnchored(update.scale, update.anchor);
        }
    }

    event->accept();
    return true;
}

// 格線與參考線畫在頁面內容之上、選取之下。
//
// 兩者都是檢視層的輔助，**不寫進 PDF**——把它們畫進內容串流會讓使用者
// 印出來或寄出去時多出一堆線，而那是不可逆的。
void PageView::paintGuides(QPainter& painter, const domain::RectI& visible) const {
    if (!guidesVisible_ && !grid_.enabled) return;

    for (const domain::PagePlacement& placement : layout_.visiblePages(visible)) {
        const domain::RectI& rect = placement.rect;
        const QRect deviceRect(rect.x - visible.x, rect.y - visible.y, rect.width, rect.height);
        const domain::PageTransform transform{controller_->pageSizePt(placement.pageIndex),
                                              renderScale(), rotation_};

        painter.save();
        painter.setClipRect(deviceRect);

        if (grid_.enabled && grid_.spacingPt > 0.0) {
            // 格線用 palette 的 Mid：深色主題下會自動變成比背景亮的線。
            QPen pen(palette().color(QPalette::Mid));
            pen.setWidth(1);
            painter.setPen(pen);
            const domain::SizeF pageSize = transform.pageSizePt();
            for (double x = 0.0; x <= pageSize.width; x += grid_.spacingPt) {
                const domain::PointF top = transform.toDevice(domain::PointF{x, pageSize.height});
                const int px = deviceRect.x() + static_cast<int>(std::lround(top.x));
                painter.drawLine(px, deviceRect.top(), px, deviceRect.bottom());
            }
            for (double y = 0.0; y <= pageSize.height; y += grid_.spacingPt) {
                const domain::PointF left = transform.toDevice(domain::PointF{0.0, y});
                const int py = deviceRect.y() + static_cast<int>(std::lround(left.y));
                painter.drawLine(deviceRect.left(), py, deviceRect.right(), py);
            }
        }

        if (guidesVisible_) {
            QPen pen(QColor(0, 140, 255));
            pen.setWidth(1);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            for (const domain::GuideLine& guide : guides_.lines()) {
                if (guide.orientation == domain::GuideOrientation::Vertical) {
                    const domain::PointF p =
                        transform.toDevice(domain::PointF{guide.positionPt, 0.0});
                    const int px = deviceRect.x() + static_cast<int>(std::lround(p.x));
                    painter.drawLine(px, deviceRect.top(), px, deviceRect.bottom());
                } else {
                    const domain::PointF p =
                        transform.toDevice(domain::PointF{0.0, guide.positionPt});
                    const int py = deviceRect.y() + static_cast<int>(std::lround(p.y));
                    painter.drawLine(deviceRect.left(), py, deviceRect.right(), py);
                }
            }
        }
        painter.restore();
    }
}

void PageView::paintSelection(QPainter& painter, const domain::RectI& visible) const {
    const app::Selection& current = selection_->selection();
    if (current.isEmpty()) return;

    const domain::RectI pageRect = layout_.pageRect(current.pageIndex);
    if (pageRect.isEmpty()) return;

    const domain::PageTransform transform(controller_->pageSizePt(current.pageIndex), scale_,
                                          rotation_);

    QColor overlay = palette().highlight().color();
    overlay.setAlpha(90);
    painter.setPen(Qt::NoPen);
    painter.setBrush(overlay);

    for (const domain::QuadPoint& quad : current.quads) {
        // quad 是頁面座標，畫之前要轉回裝置座標；旋轉時四個角各自轉換，
        // 不能只轉對角再組矩形。
        QPolygonF polygon;
        for (const domain::PointF& corner : quad.corners()) {
            const domain::PointF device = transform.toDevice(corner);
            polygon << QPointF(pageRect.x + device.x - visible.x,
                               pageRect.y + device.y - visible.y);
        }
        painter.drawPolygon(polygon);
    }
}

void PageView::paintEvent(QPaintEvent* event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), palette().mid());

    if (!controller_->isOpen()) {
        painter.setPen(palette().text().color());
        painter.drawText(viewport()->rect(), Qt::AlignCenter,
                         tr("開啟一份 PDF 以開始審閱（Ctrl+O）"));
        return;
    }

    const domain::RectI visible = visibleDocumentRect();
    // 縮放中畫的是粗略倍率的圖磚（會被拉伸），停止後畫的是精確倍率的圖磚（比例為 1）。
    const double render = renderScale();
    const std::int32_t scaleKey =
        zooming_ ? domain::coarseScaleKey(render) : domain::exactScaleKey(render);
    // 圖磚是以渲染倍率算的，畫的時候換回邏輯像素——多除一個 DPI 係數就是這件事。
    const double tileToActual = scale_ / domain::scaleOfKey(scaleKey);

    for (const domain::PagePlacement& placement :
         layout_.visiblePages(visible)) {
        const domain::RectI& pageRect = placement.rect;
        const QRect pageOnScreen(pageRect.x - visible.x, pageRect.y - visible.y, pageRect.width,
                                 pageRect.height);

        // 頁面底色。PDF 頁面本身不保證畫背景，沒有這一塊會看到桌面灰。
        //
        // 這是全案刻意保留的硬編顏色，理由是它不是介面元素而是「紙」：
        // PDFium 產生的圖磚已經把白底烘進像素裡，這裡改成隨主題走的顏色，
        // 只會讓圖磚尚未到位的區域與已到位的區域顏色不一致，看起來像破圖。
        // 高對比模式下要反轉的是**文件內容**，那屬於引擎層的渲染選項
        // （PRD-VIEW 的夜間模式），不是在呈現層改一塊底色能達成的。
        //
        // 透明度格線模式（PRD-VIEW-018）是例外：這時圖磚本身就是刻意帶著
        // 真正的 alpha 通道渲染出來的（engine::RenderOptions::transparencyGrid），
        // 頁面沒畫到的地方 alpha = 0。底色如果還是純白，透明區域跟白色內容
        // 會混在一起分不出來，因此改畫棋盤格，讓圖磚疊上去之後真正透明的
        // 地方能透出格線。
        if (controller_->transparencyGrid()) {
            paintTransparencyCheckerboard(painter, pageOnScreen);
        } else {
            painter.fillRect(pageOnScreen, Qt::white);
        }
        painter.setPen(palette().dark().color());
        painter.drawRect(pageOnScreen.adjusted(0, 0, -1, -1));

        const std::int32_t maxCol = (pageRect.width - 1) / domain::kTileSize;
        const std::int32_t maxRow = (pageRect.height - 1) / domain::kTileSize;
        const std::int32_t firstCol = std::max(0, (visible.x - pageRect.x) / domain::kTileSize);
        const std::int32_t firstRow = std::max(0, (visible.y - pageRect.y) / domain::kTileSize);
        const std::int32_t lastCol =
            std::min(maxCol, (visible.right() - pageRect.x) / domain::kTileSize);
        const std::int32_t lastRow =
            std::min(maxRow, (visible.bottom() - pageRect.y) / domain::kTileSize);

        for (std::int32_t row = firstRow; row <= lastRow; ++row) {
            for (std::int32_t col = firstCol; col <= lastCol; ++col) {
                const domain::TileKey key{placement.pageIndex, scaleKey, col,      row,
                                          rotation_,            controller_->nightMode()};
                const QImage tile = controller_->tileIfReady(key);
                if (tile.isNull()) continue;

                // 圖磚以 bucket 倍率算成，畫的時候按實際倍率貼上。
                // 這是縮放過程中的暫時畫面；縮放停止 80 毫秒後會以精確倍率重算取代
                // （PRD §4.3 明令不得以拉伸結果作為最終畫面）。
                const QRectF target(
                    pageRect.x - visible.x + col * domain::kTileSize * tileToActual,
                    pageRect.y - visible.y + row * domain::kTileSize * tileToActual,
                    domain::kTileSize * tileToActual, domain::kTileSize * tileToActual);

                // 裁掉超出頁面的部分，避免圖磚的白邊蓋到頁面外的桌面。
                painter.save();
                painter.setClipRect(pageOnScreen);
                painter.drawImage(target, tile);
                painter.restore();
            }
        }
    }

    paintGuides(painter, visible);
    paintPendingVertices(painter, visible);
    paintPendingStroke(painter, visible);
    paintFormFieldHighlights(painter, visible);
    // 強調框畫在最後：它是選取回饋，必須蓋在所有內容之上才看得見。
    paintHighlightedRect(painter, visible);
    paintSelection(painter, visible);

    // 拖曳中的形狀預覽。畫在最後，蓋在圖磚與選取之上。
    if (shapePage_ >= 0) {
        const domain::RectI pageRect = layout_.pageRect(shapePage_);
        const domain::PageTransform transform(controller_->pageSizePt(shapePage_), scale_,
                                              rotation_);
        const domain::PointF a = transform.toDevice(shapeAnchor_);
        const domain::PointF b = transform.toDevice(shapeCurrent_);
        const QRectF preview(QPointF(pageRect.x + std::min(a.x, b.x) - visible.x,
                                     pageRect.y + std::min(a.y, b.y) - visible.y),
                             QSizeF(std::abs(b.x - a.x), std::abs(b.y - a.y)));

        painter.setPen(QPen(palette().highlight().color(), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        if (tool_ == Tool::Ellipse) {
            painter.drawEllipse(preview);
        } else {
            painter.drawRect(preview);
        }
    }
}

}  // namespace alioth::ui
