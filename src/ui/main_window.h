#pragma once

// 主視窗。Ribbon 為自研（PRD-UI-002），M0 階段先以工具列骨架佔位，
// 待 WBS 3.2 接手；此處刻意不引入第三方 Ribbon 元件。

#include <QColor>
#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QString>

#include "app/annotation_service.h"
#include "app/form_controller.h"
#include "app/uisystem/shortcut_scheme.h"
#include "app/uisystem/tab_layout_model.h"
#include "app/uisystem/tool_persistence.h"
#include "app/uisystem/theme_controller.h"
#include "app/history_store.h"
#include "app/navigation_service.h"
#include "app/view_history.h"
#include "platform/external_change.h"
#include "domain/ink_smoothing.h"
#include "domain/annotation.h"
#include "domain/measurement.h"
#include "domain/presentation.h"
#include "domain/document.h"
#include "app/page_operations_service.h"
#include "app/form_build_service.h"
#include "app/stamp_service.h"
#include "ui/stamp_dialog.h"

#include <functional>
#include <vector>

class QLabel;
class QLineEdit;
class QDockWidget;
class QSplitter;
class QListWidget;
class QTreeWidget;
class QTimer;
class QToolBar;
class QCheckBox;
class QComboBox;

namespace alioth::app {
class AccessibilityService;
class AnnotationService;
class CommandStack;
class PageOperationsService;
class RedactionService;
class AnnotationFlattenService;
class AttachmentService;
class ReadAloudController;
class SignatureController;
class Settings;
class DocumentController;
class SelectionController;
struct ExternalTool;
}

namespace alioth::ui {

class AnnotationPropertiesPanel;
class AttachmentsPanel;
class DestinationsPanel;
class FieldsPanel;
class HistoryPanel;
class LinksPanel;
class LayersPanel;
class PageView;
class SignaturePanel;
class TagsPanel;
class OrderPanel;
class InspectorPanel;
class PanZoomPanel;
class LoupeWidget;
class RulerWidget;

namespace ribbon {
class ActionRegistry;
class RibbonBar;
}  // namespace ribbon

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // 這個視窗是不是主視窗。
    //
    // 工作階段（開了哪些文件、視窗幾何、面板版面）只屬於主視窗：分離出來的
    // 視窗若也還原與寫入工作階段，「在新視窗開啟」會先把上次的頁籤全部開進
    // 新視窗，而關掉它又會把主視窗那一組蓋掉。
    enum class WindowRole { Primary, Detached };

    explicit MainWindow(QWidget* parent = nullptr);
    explicit MainWindow(WindowRole role, QWidget* parent = nullptr);
    ~MainWindow() override;

    void openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

    // 拖放開檔（PRD-IO-006）。
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // 宣告順序就是初始化順序，而建構式的初始化列表以它為準——這個旗標
    // 必須排在所有成員之前，因為建構過程中就會用到它。
    WindowRole role_{WindowRole::Primary};

    void buildActions();
    void buildDockPanels();
    // 首次啟動的面板預設：只留左側導覽，其餘收起來。
    void applyDefaultPanelLayout();
    // 離散跳轉（面板、搜尋命中、連結）。記進瀏覽歷史後才跳。
    // 連續捲動與縮放**不**走這裡——每次捲動都記一筆的話，後退鍵只會退回
    // 幾十像素，使用者要按上百次才回得到原本的位置，等於這個功能不存在。
    void navigateToPage(int pageIndex);
    // PRD-IO-007：匯出目前頁面為影像、匯出整份純文字。
    void exportCurrentPageImage();
    void exportDocumentText();
    // PRD-TXT-003：框選區域的文字複製為 TSV。
    // PRD-TXT-002：複製選取的文字（純文字 + 富文字）。
    void copySelectionToClipboard();
    void copyAreaTextAsTsv(int pageIndex, const domain::RectF& pageRect);
    // PRD-TXT-005：框選區域渲染成影像複製到剪貼簿。
    void copySnapshotToClipboard(int pageIndex, const domain::RectF& pageRect);
    // PRD-PAGE-013：編輯頁面標籤（編號範圍）。
    void editPageLabels();
    // PRD-ANN-010：刪除註解清單上選取的那一則，走命令堆疊可復原。
    void deleteSelectedAnnotation();
    // 註釋視窗（PRD-ANN-004）。index 是 controller_->annotations() 的索引，
    // 不是清單列號——清單經過篩選與排序，兩者不同。
    void openNotePopup(std::size_t index);
    // 頁面上被點到的那一則註解。找不到時回傳 annotations().size()。
    // 命中時取最上層（最後寫入）的那一則：後畫的蓋在先畫的上面。
    [[nodiscard]] std::size_t annotationAt(int pageIndex, const domain::PointF& pagePoint) const;
    // 選取註解工具的命中測試：認所有子型，不只便利貼。
    //
    // 與 annotationAt() 分開是因為兩者服務的意圖相反：annotationAt() 是
    // 「選取文字時順便看看點到哪則便利貼」，所以刻意只認便利貼；這一個是
    // 「使用者明說要選註解」，所以全部都要認。
    [[nodiscard]] std::size_t annotationAtForSelection(int pageIndex,
                                                       const domain::PointF& pagePoint) const;
    // 選中某一則註解：畫強調框、在清單上選起來、跳到那一頁。
    // index 超出範圍代表取消選取。
    void selectAnnotation(std::size_t index);
    // 註解屬性面板（PRD-ANN-009）。
    //
    // 面板要的是完整的 domain::Annotation，而清單上那一份是 AnnotationSummary
    // ——摘要裡沒有顏色、線寬、旗標。完整內容從檔案讀回來，用與複製註解
    // 相同的一組定位座標比對。
    void loadAnnotationProperties(std::size_t index);
    void applyAnnotationProperties(const domain::Annotation& edited);
    // 上一則／下一則註解（PDF-XChange 的 Previous / Next Comment）。
    // 走的是文件順序（頁碼、頁內序號），不是清單目前的排序——使用者按
    // 「下一則」時期待的是文件裡的下一則，不是「依作者排序後的下一列」。
    void goToAdjacentAnnotation(int direction);
    std::size_t selectedAnnotation_{static_cast<std::size_t>(-1)};
    void commitNoteContents(int pageIndex, int indexOnPage, const QString& contents);
    // 塗黑（PRD-ANN-032 / PRD-ANN-033）。標記可逆、套用不可逆，兩者刻意
    // 分成兩個動作而不是一個帶確認的動作——把它們合併，使用者一次按錯就
    // 永久刪掉了內容。
    void markRedaction(int pageIndex, const domain::RectF& pageRect);
    void clearRedactionMarks();
    // 清除隱藏中繼資料（PRD-ANN-034）。與塗黑是兩個獨立操作。
    void sanitizeDocument();
    // 頁首頁尾／浮水印／Bates（PRD-PAGE-004）。一個對話框三種預設。
    void applyStamp(StampDialog::Preset preset, const QString& label);
    // 註解畫完之後把工具切回選取（除非開了持續模式）。PRD-UI-007。
    void applyToolPersistence();
    // 儲存那一組（ADR-008）。每一次修改都當下就寫進磁碟，因此沒有
    // 「未存檔的修改」——另存是分岔出一份副本，復原是把命令堆疊倒回開頭。
    void saveDocumentAs();
    void revertAllChanges();
    // 比較文件（PRD-CMP-001）。不修改任何東西，因此不進命令堆疊。
    void compareWithDocument();
    // 合併與分割（PRD-PAGE-002）。兩者都另存新檔，不動任何來源。
    void mergeDocuments();
    void splitDocument();
    // 刪除／擷取頁面（PRD-PAGE-002）。頁碼以「1,3,5-8」的範圍寫法輸入。
    // 安全性（PRD-SEC-002）。讀出加密演算法與權限、移除密碼都可行；
    // 設定新密碼在預編譯 PDFium 的公開 API 下不可行，明確回報而不是假裝。
    void showSecurityInfo();
    void setDocumentPassword();
    void removeDocumentSecurity();
    // 取得（必要時建立）某一份文件的復原堆疊；關掉頁籤時丟掉它。
    [[nodiscard]] app::CommandStack* stackFor(const QString& path);
    void dropStackFor(const QString& path);
    void movePagesByRange();
    void rotatePagesByRange(const QString& label, domain::pages::PageRotation rotation);
    void deletePagesByRange();
    void duplicatePagesByRange();  // PDF-XChange 的 Duplicate Page
    void extractPagesByRange();
    // 解析使用者輸入的頁碼範圍，回傳 0 起算的頁碼；無效或超出範圍時回傳空。
    [[nodiscard]] std::vector<int> parsePageRange(const QString& text) const;
    // 檔案附件註解（PRD-ANN-015）：框出圖示位置後再問要附哪個檔案。
    void attachFileAt(int pageIndex, const domain::RectF& pageRect);
    // 表單資料（PRD-FORM-002）：匯出／匯入／重設。
    void exportFormData();
    void importFormData();
    void resetFormFields();
    // 建立表單欄位（PRD-FORM-001~）。選單只切換「接下來要畫哪一種」，
    // 真正建立發生在使用者框出矩形之後——欄位的位置與大小是它的一部分，
    // 用對話框輸入座標沒有人會用。
    void createFormField(int pageIndex, const domain::RectF& pageRect);
    engine::formbuild::BuildFieldType pendingFieldType_{
        engine::formbuild::BuildFieldType::Text};
    // 數位簽署（PRD-SIG-004）。增量附加，原檔位元組不動，既有簽章仍有效。
    void signCurrentDocument();
    void applyRedactionMarks();
    // 回覆註解或替它標上審閱狀態（PRD-ANN-007）。state 留空代表純回覆。
    void replyToSelectedAnnotation(const QString& state);
    // 複製／貼上註解（PRD-ANN-011），含跨文件與跨視窗。
    void copySelectedAnnotation();
    void pasteAnnotations();
    // 鉛筆筆畫（PRD-ANN-003）：平滑 → 依壓力切成數個線寬層 → 一次寫成
    // 數則 /Ink。/Ink 只有單一 /BS /W，變寬的筆畫只能用多則註解模擬。
    void applyPencilStroke(int pageIndex, const std::vector<domain::PressurePoint>& points);
    // 註解摘要（PRD-ANN-028）。「僅摘要」輸出一份純文字；另外兩種產生一份
    // 新的 PDF——「文件加摘要」在每頁後插入摘要頁，「並排」把每頁與它的摘要
    // 併成一張（左原文、右摘要）。三者都刻意不動原檔。
    void exportCommentSummary();
    void flattenAnnotations();  // PRD-ANN-013
    enum class SummaryLayout { InsertAfterEachPage, SideBySide };
    void exportDocumentWithSummary(SummaryLayout layout);
    // 註解交換（PRD-ANN-013）。格式依檔案內容判定，不依副檔名。
    void importAnnotationsFromFile();
    // 匯出全部或只匯出清單裡選定的那幾則。兩者是選單上的兩個項目而不是
    // 一個會依選取狀態變聰明的項目：同一個入口產出兩種不同的檔案，
    // 而使用者不見得記得自己在清單裡點過什麼。
    enum class ExportScope { All, SelectedOnly };
    void exportAnnotationsToFile(ExportScope scope);
    // 建立新文件（PDF-XChange 的 File → New Document）。三條路徑的引擎側
    // 早就完成並測過，缺的一直是入口。
    void createBlankDocument();
    void createDocumentFromTextFile();
    void createDocumentFromImages();
    [[nodiscard]] bool writeAndOpenNewDocument(const QString& title, const std::string& bytes,
                                               const QString& suggestedName);
    // 寄出所有開啟的文件（PDF-XChange 的 Email All Open Documents）。
    void manageTrustedCertificates();  // PDF-XChange 的 Digital IDs
    void auditSpaceUsage();  // PDF-XChange 的 Audit Space Usage
    void clearAllSignatures();  // PDF-XChange 的 Clear all Signatures
    void emailAllOpenDocuments();
    // 設定的匯出／匯入／重設（PDF-XChange 的 Manage Settings）。
    void exportSettingsProfile();
    void importSettingsProfile();
    void resetSettingsProfile();
    // PRD-UI-017：把磁碟上的檔案改名，並重新開啟新路徑。
    void renameCurrentDocument();
    // PRD-ZOOM-003 Fit Visible：忽略白邊，縮放到實際內容範圍。
    void fitVisible();
    void updateNavigationActions();
    // PRD-SEC-001：依文件權限旗標灰化對應動作。
    void applyPermissionRestrictions();
    void buildStatusBar();
    void buildPanelActions();
    // F6 在面板之間循環（PRD-A11Y-005，docs/UX_CONCEPT.md §6）。
    //
    // Tab 在停靠面板林立的視窗裡不夠用：每個面板裡的清單都會吃掉 Tab
    // （或需要幾十次 Tab 才走得完），使用者實際上被困在第一個面板裡。
    // F6 是 Windows 桌面應用的既定慣例，螢幕閱讀器使用者會直接去按。
    void focusNextPanel(bool backwards);
    // 目前可見且可接受焦點的面板順序。順序以畫面位置為準而不是建立順序，
    // 否則 Tab 順序會與視覺順序不一致，那正是無障礙檢查最常見的失分項。
    [[nodiscard]] std::vector<QWidget*> focusCycleTargets() const;
    void updateWindowTitle(const QString& path);
    void populateOutline();
    // 關檔後把所有「從文件來的」介面狀態清乾淨。集中在一處而不是散在
    // 每個關閉路徑上：關閉有兩條路（選單的關閉、關掉最後一個頁籤），
    // 各寫一份必然會漏掉其中一條。
    void clearDocumentUi();
    // 重載（寫入之後）要更新的東西。與開檔的處理器刻意分開：開檔會重設
    // 檢視位置、寫開啟記錄、重建縮圖清單，而那三件事在重載時全都是錯的。
    // 縮圖拖放之後真正執行移動。延到事件迴圈下一輪並先問使用者——
    // 拖曳最容易誤觸，而移動頁面會重寫整份檔案。
    void commitThumbnailReorder(int from, int to);
    void refreshAfterReload();
    // 重載後把縮圖換成新的內容，但不動清單本身（不 clear、不捲回頂端）。
    void refreshThumbnailImages();
    // 目前頁附近的註解。捲到哪補到哪——控制器會跳過掃過的頁，
    // 所以這個函式可以在每次換頁時無條件呼叫。
    void requestAnnotationsAround(int pageIndex);
    void populateThumbnails();
    // 依縮圖面板目前看得到的範圍補要縮圖。捲動時呼叫。
    void syncThumbnailSelection(int pageIndex);
    void requestVisibleThumbnails();
    void runSearch();
    void populateAnnotations();

    void applyHighlight();
    void followLink(const domain::LinkTarget& target);
    void restoreSession();
    // 歷史與續讀位置（PRD-NAV-005、PRD-NAV-009）。
    void recordHistory(const QString& path);
    void storeReadingPosition();
    void reloadNavigationPanels(const QString& path);
    void toggleAutoScroll();
    // 把鍵位表套到已註冊的動作上。動作是分階段註冊的，因此這件事要在最後做。
    void applyShortcutScheme();
    void saveAttachment(int index, const QString& suggestedName);
    void storeSession();
    void buildOrganizeMenu(class QMenu* menu);
    // 全檔重寫的操作在動手前必須讓使用者知道簽章會失效。
    [[nodiscard]] bool confirmRewrite(const QString& operation);
    // PRD-IO-004：寫入前確認檔案沒有被別的程式動過。
    // 回傳 false 代表使用者取消，或已經改走重新載入。
    [[nodiscard]] bool confirmNoExternalChange(const QString& operation);
    void commitPageOperation(const QString& label, std::function<app::PageOperationResult()> run);
    // PRD-PAGE-003 頁面尺寸調整。問兩件事：目標紙張，以及內容要不要跟著縮放。
    void resizePagesWithDialog();
    void applyTextMarkup(domain::TextMarkupKind kind, const QString& label);
    void applyShape(int pageIndex, const domain::RectF& pageRect);
    void commitAnnotation(app::AnnotationRequest request, const QString& label);
    // PRD-A11Y-004：從 Tags 面板的右鍵選單觸發，跳出對話框讓使用者輸入替代文字。
    void editAlternateText(int structElementObjectNumber, const QString& currentAltText);
    [[nodiscard]] QString annotationAuthor() const;
    void reloadCurrentDocument();
    void updateUndoActions();
    void rebuildRecentMenu();
    void applySettings();
    void printDocument();
    // 列印預覽（PRD-IO-008）。與列印共用同一個 PrintService。
    void showPrintPreview();
    // 分割檢視（PRD-UI-018 / PRD-VIEW-017）。
    //
    // 試算表式分割是四格而不是「兩次兩格」：Excel 的分割條同時橫豎切開，
    // 四格共用兩條分隔線。用兩個獨立的兩格分割拼出來的話，上下兩排的
    // 直向分隔線可以各自拖到不同位置，那不是使用者要的「對齊的四格」。
    enum class SplitMode { None, Horizontal, Vertical, Quad };
    void setSplitMode(SplitMode mode);

    // 多頁籤（PRD-UI-001）。
    //
    // 一個視窗只有一個 DocumentController，切換頁籤就是換這個控制器開的檔案。
    // 不是每個頁籤各持一份文件把手：PDFium 非執行緒安全，每份文件都要一條
    // 專屬執行緒（CLAUDE.md 的四個硬性限制之一），十個頁籤就是十條執行緒與
    // 十份圖磚快取。只有看得見的那一份需要渲染，其餘只需要記住閱讀位置——
    // 而閱讀位置本來就存在 HistoryStore 裡。
    //
    // 狀態的真相在 app::TabLayoutModel（已有 11 例測試），QTabBar 是薄殼。
    // 真正把檔案交給控制器。openPath() 負責頁籤簿記之後呼叫它；
    // 切換頁籤只需要換檔案，不該再跑一次「要不要新增頁籤」。
    void loadDocument(const QString& path);
    void closeTab(int index);
    void syncTabBar();
    // 把某個頁籤拖出來變成獨立視窗（PRD-UI-001 的分離）。
    void detachTab(int index);
    // Ribbon 與傳統選單並存，隨時可切換（PRD-UI-002）。
    // 兩者共用同一組 QAction，不是兩套實作——那會讓「某個功能只有選單有」變成常態。
    void buildRibbon();
    void setRibbonVisible(bool visible);
    void customizeRibbon();
    // PRD-VIEW-009：把 PresentationController 的狀態映射成視窗與介面可見性。
    void applyViewMode();
    void registerRibbonAction(const QString& id, QAction* action);

    // PRD-IO-011：把目前文件當附件交給系統郵件用戶端。
    void emailCurrentDocument();
    // PRD-UI-016：第三方程式工具列。工具清單變動（新增／編輯／移除）後
    // 重建，而不是每次啟動都重新掃一次 Settings。
    void rebuildExternalToolsToolbar();
    void launchExternalTool(const app::ExternalTool& tool);

    app::DocumentController* controller_{nullptr};
    app::SelectionController* selection_{nullptr};
    app::AnnotationService* annotations_{nullptr};
    app::AccessibilityService* accessibilityService_{nullptr};
    app::ReadAloudController* readAloud_{nullptr};
    app::CommandStack* commands_{nullptr};
    // 每個頁籤各自一份（UI-23）。切到別的文件再切回來，復原步驟還在。
    app::CommandStack* defaultCommands_{nullptr};
    QHash<QString, app::CommandStack*> commandStacks_;
    app::PageOperationsService* pageOps_{nullptr};
    app::SignatureController* signatures_{nullptr};
    app::RedactionService* redaction_{nullptr};
    app::AnnotationFlattenService* annotationFlatten_{nullptr};
    app::StampService* stamps_{nullptr};
    app::FormBuildService* formBuild_{nullptr};
    app::AttachmentService* attachmentService_{nullptr};
    // 量測比例尺（PRD-ANN-024）。第一次用量測工具時由使用者校正，之後沿用。
    // 未校正時算不出真實長度，那時寧可先問也不寫出一個看似合理的假數字。
    domain::MeasureInfo measureScale_{};
    // 工具持續模式（PRD-UI-007）。規則本身是一個純函數，狀態存在偏好設定裡。
    app::ToolPersistenceModel toolPersistence_{};
    SignaturePanel* signaturePanel_{nullptr};
    QDockWidget* signatureDock_{nullptr};
    HistoryPanel* historyPanel_{nullptr};
    QDockWidget* historyDock_{nullptr};
    DestinationsPanel* destinationsPanel_{nullptr};
    QDockWidget* destinationsDock_{nullptr};
    LinksPanel* linksPanel_{nullptr};
    QDockWidget* linksDock_{nullptr};
    AttachmentsPanel* attachmentsPanel_{nullptr};
    QDockWidget* attachmentsDock_{nullptr};
    LayersPanel* layersPanel_{nullptr};
    QDockWidget* layersDock_{nullptr};
    FieldsPanel* fieldsPanel_{nullptr};
    QDockWidget* fieldsDock_{nullptr};
    AnnotationPropertiesPanel* propertiesPanel_{nullptr};
    QDockWidget* propertiesDock_{nullptr};
    app::FormController* forms_{nullptr};
    app::ThemeController* theme_{nullptr};
    app::ShortcutScheme shortcuts_;
    TagsPanel* tagsPanel_{nullptr};
    QDockWidget* tagsDock_{nullptr};
    OrderPanel* orderPanel_{nullptr};
    QDockWidget* orderDock_{nullptr};
    PanZoomPanel* panZoomPanel_{nullptr};
    QDockWidget* panZoomDock_{nullptr};
    LoupeWidget* loupePanel_{nullptr};
    QDockWidget* loupeDock_{nullptr};
    InspectorPanel* inspectorPanel_{nullptr};
    QDockWidget* inspectorDock_{nullptr};
    app::HistoryStore history_;
    app::ViewHistory viewHistory_;
    // 開檔（或上次成功寫入）當下的檔案快照，供 PRD-IO-004 比對。
    platform::FileSnapshot fileSnapshot_;
    QAction* backAction_{nullptr};
    QAction* forwardAction_{nullptr};
    app::NavigationService navigation_;
    app::AutoScroller autoScroller_;
    QTimer* autoScrollTimer_{nullptr};
    // 開檔是非同步的，續讀位置要等文件真的開好才能套用。
    int pendingRestorePage_{-1};
    double pendingRestoreScale_{0.0};
    app::Settings* settings_{nullptr};
    class QMenu* recentMenu_{nullptr};
    // 註解選單在 buildActions() 建立，但刪除／回覆／設定狀態／複製註解這幾個
    // 動作要等 buildDockPanels() 有了註解清單才建得出來（它們的 parent 必須是
    // 清單，快捷鍵才不會與頁面上的 Delete 互搶）。留著指標讓後者補進同一個選單，
    // 否則切到傳統選單的使用者在選單列上完全找不到刪除註解。
    class QMenu* commentMenu_{nullptr};
    // 同樣的理由：「複製註解」與既有的剪下／貼上註解同屬編輯選單，
    // 但它要等註解清單建好才生得出來，所以留著選單與插入位置。
    class QMenu* editMenu_{nullptr};
    class QAction* editCutAction_{nullptr};
    class QAction* undoAction_{nullptr};
    class QAction* redoAction_{nullptr};
    QString currentPath_;
    PageView* pageView_{nullptr};
    // 主檢視以外的檢視。分割關閉時為空；水平／垂直分割一個；四格分割三個。
    // secondView_ 是其中的第一個，保留這個名字是因為設定套用等處只需要
    // 「第二個檢視」的概念。
    PageView* secondView_{nullptr};
    std::vector<PageView*> extraViews_;
    QSplitter* splitter_{nullptr};
    // 四格分割用的兩排水平分割器。非四格模式時為 nullptr。
    QSplitter* topRow_{nullptr};
    QSplitter* bottomRow_{nullptr};
    SplitMode splitMode_{SplitMode::None};
    class QTabBar* documentTabs_{nullptr};
    app::TabLayoutModel tabs_;
    // syncTabBar() 會呼叫 setCurrentIndex，那會再發出 currentChanged。
    // 沒有這道閘，換頁籤就會遞迴開檔。
    bool syncingTabs_{false};
    ribbon::ActionRegistry* actionRegistry_{nullptr};
    ribbon::RibbonBar* ribbon_{nullptr};
    // Ribbon 的容器。放在頂部工具列區才能橫跨整個視窗寬度並壓在停靠區之上。
    QToolBar* ribbonHost_{nullptr};
    QLabel* pageLabel_{nullptr};
    QComboBox* zoomBox_{nullptr};
    QCheckBox* searchMatchCase_{nullptr};
    QCheckBox* searchWholeWord_{nullptr};
    // 頁面尺寸與游標位置。游標離開頁面時只留尺寸——把座標留在最後一個值上
    // 會讓使用者以為游標還在那裡。
    QLabel* geometryLabel_{nullptr};
    void updateGeometryLabel(int pageIndex, const domain::PointF* pagePoint);
    QLabel* noticeLabel_{nullptr};
    QTreeWidget* outlineTree_{nullptr};
    QListWidget* thumbnailList_{nullptr};
    // 已經送出過請求的頁。捲動是連續事件，不去重的話同一頁會被排進佇列幾十次。
    QSet<int> requestedThumbnails_;
    QListWidget* searchResults_{nullptr};
    QListWidget* annotationList_{nullptr};
    QComboBox* annotationAuthorFilter_{nullptr};
    QComboBox* annotationTypeFilter_{nullptr};
    QComboBox* annotationSortKey_{nullptr};
    QLineEdit* annotationSearch_{nullptr};
    // 篩選排序後，清單第 i 列對應 controller_->annotations() 的哪一個索引。
    std::vector<std::size_t> annotationOrder_;
    // 面板收起來時選到的那一則。打開面板的當下要補讀一次，否則面板會停在
    // 「沒有選取」，而使用者明明剛剛才點過一則註解。
    std::size_t propertiesPending_{static_cast<std::size_t>(-1)};
    // 上一次選的圖章樣式。連續蓋同一種是最常見的用法。
    int lastStampChoice_{0};
    class StickyNotePopup* notePopup_{nullptr};
    // 縮圖清單正在因為重排而重建。擋掉 currentRowChanged 觸發的跳頁，
    // 否則重建過程中的每一次選取變化都會把使用者帶到別的頁。
    bool reorderingThumbnails_{false};
    // 縮圖選取正在跟著檢視區走。捲動換頁時要把選取移過去，但那一次
    // setCurrentRow 不該再回頭叫 navigateToPage——那會把使用者捲到一半的
    // 位置硬拉到頁首，捲動就變成一格一格跳。
    bool syncingThumbnailSelection_{false};
    QDockWidget* thumbnailDock_{nullptr};
    QDockWidget* bookmarkDock_{nullptr};
    QDockWidget* commentDock_{nullptr};
    QDockWidget* searchDock_{nullptr};
    QLineEdit* searchField_{nullptr};
    // PRD-UI-016：第三方程式工具列。nullptr 代表清單目前是空的
    // （沒有工具可以啟動時不顯示一條空工具列，那只會佔位又沒有內容）。
    QToolBar* externalToolsToolbar_{nullptr};
    // M0 時期的縮放工具列。Ribbon 上線後它的每一顆按鈕都在「檢視」分頁重複了一次，
    // 因此只在傳統選單模式下顯示。動作本身保留：選單與 Ribbon 都引用同一批。
    QToolBar* viewToolBar_{nullptr};
    RulerWidget* horizontalRuler_{nullptr};
    RulerWidget* verticalRuler_{nullptr};
    QWidget* rulerCorner_{nullptr};
    domain::PresentationController presentation_;
    // 進簡報模式前是開著的面板。退出時要原樣還原——
    // 使用者排好的版面不該因為看了一次簡報就消失。
    std::vector<QDockWidget*> panelsHiddenForPresentation_;
    bool ribbonWasVisible_{true};
    // 自訂配色的目前選擇。預設是「淺米底 + 深灰字」——純白配純黑正是
    // 使用者想避開的組合，開啟對話框時給一個已經有用的起點。
    QColor customBackground_{0xF5, 0xF0, 0xE1};
    QColor customText_{0x2B, 0x2B, 0x2B};
};

}  // namespace alioth::ui
