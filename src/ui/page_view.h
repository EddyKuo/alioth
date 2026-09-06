#pragma once

// PDF 檢視元件。只做三件事：算可視區、把圖磚貼上畫面、把使用者輸入翻成可視區變更。
// 它不認識 PDFium，也不認識檔案——一切經由 DocumentController。

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QHash>

#include <utility>
#include <vector>

#include "app/document_controller.h"
#include "app/selection_controller.h"
#include "app/touch/touch_gestures.h"
#include "app/uisystem/cursor_scale.h"
#include "domain/geometry.h"
#include "domain/guides.h"
#include "domain/ink_smoothing.h"
#include "domain/page_layout.h"

class QTimer;

namespace alioth::ui {

// 目前作用中的工具。選取以外的工具會把拖曳解讀成畫一則註解，
// 而不是選文字——這是使用者對「工具」最直接的期待。
enum class Tool {
    Select,
    Hand,  // 拖曳平移。不選字、不畫東西，只捲動。
    Highlight,
    Underline,
    StrikeOut,
    Rectangle,
    Ellipse,
    StickyNote,
    // 框選一塊區域再做事，兩者都不產生註解（PRD-TXT-003 / PRD-TXT-005）。
    // 與 Rectangle 走同一條拖曳路徑，差別只在放開時做什麼。
    AreaSelect,  // 區域內的文字複製為 TSV
    Snapshot,    // 區域渲染成影像複製到剪貼簿
    // 框出一塊待塗黑的區域（PRD-ANN-033）。走與 AreaSelect 相同的拖曳路徑，
    // 差別只在放開時做什麼——標記本身是一則 /Redact 註解，可逆。
    RedactMark,
    // 框出一個表單欄位的位置（PRD-FORM-001~）。同樣走 AreaSelect 的拖曳路徑；
    // 要建立哪一種欄位由呼叫端（MainWindow）記著，PageView 不認識表單。
    FormField,
    // 拖曳一次畫一條（PRD-ANN-002）。
    Line,
    Arrow,
    // 逐點點擊，雙擊或 Enter 結束、Esc 取消（PRD-ANN-002）。
    // 與拖曳類分開是因為互動方式根本不同：多邊形的頂點數是使用者決定的，
    // 沒辦法用「按下、拖曳、放開」表達。
    Polygon,
    PolyLine,
    // 雲線：與多邊形同一條收集路徑，差別只在寫出時帶 /BE /S /C。
    Cloud,
    // 註解家族其餘工具（WP24）。全部走 shapeDrawn 那條拖曳路徑——它們的
    // 共同點是「拖出一個矩形」，差別只在放開時建立哪一種註解，那是應用層
    // 的判斷。PageView 因此不必為每一種註解各長一條分支。
    TextBox,
    Typewriter,
    Callout,
    Stamp,
    Squiggly,
    Caret,
    // 檔案附件註解（PRD-ANN-015）。框出圖示的位置，放開後再問要附哪個檔案。
    Attachment,
    // 量測（PRD-ANN-024~027）。拖出一條線，套上已校正的比例尺算出距離。
    Measure,
    // 鉛筆（PRD-ANN-003）。拖曳過程逐點收集，並記下每一點的壓力——
    // 有數位板時取自 QTabletEvent，滑鼠一律回報 1.0。
    Pencil,
};

class PageView : public QAbstractScrollArea {
    Q_OBJECT

public:
    PageView(app::DocumentController* controller, app::SelectionController* selection,
             QWidget* parent = nullptr);

    void setPageIndex(std::int32_t index);
    [[nodiscard]] std::int32_t pageIndex() const noexcept { return pageIndex_; }

    void setScale(double scale);
    [[nodiscard]] double scale() const noexcept { return scale_; }

    void zoomIn();
    void zoomOut();
    void fitPage();
    void fitWidth();
    void actualSize();

    void setRotation(domain::Rotation rotation);
    [[nodiscard]] domain::Rotation rotation() const noexcept { return rotation_; }

    void setLayoutOptions(const domain::LayoutOptions& options);
    // 版面歸檢視所有而不是控制器：Split View 的兩個檢視各自縮放，
    // 而版面是縮放的函數（見 docs/SDD.md §2）。
    [[nodiscard]] const domain::PageLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] const domain::LayoutOptions& layoutOptions() const noexcept {
        return layoutOptions_;
    }

    void scrollToPage(std::int32_t index);

    // 以像素捲動（自動捲動 PRD-NAV-008 用）。已到端點時回傳 false，
    // 讓呼叫端停下來而不是繼續空轉。
    bool scrollByPixels(int delta);

    void setTool(Tool tool);
    [[nodiscard]] Tool tool() const noexcept { return tool_; }

    // 可調整游標大小（PRD-UI-018）。系統的「指標大小」設定只會縮放系統內建
    // 游標，套用形狀工具時我們自畫的十字準星必須自己依這個等級重畫。
    void setCursorSizeLevel(app::CursorSizeLevel level);
    [[nodiscard]] app::CursorSizeLevel cursorSizeLevel() const noexcept {
        return cursorSizeLevel_;
    }

    // 頁面命中測試：畫面座標 → (頁碼, 頁面座標)。找不到頁面時回傳 pageIndex = -1。
    struct PageHit {
        std::int32_t pageIndex{-1};
        domain::PointF pagePoint{};
    };
    [[nodiscard]] PageHit hitTest(const QPoint& widgetPoint) const;

    // 螢幕閱讀器看到的狀態字串（PRD-A11Y-005）：「第 3 頁，共 10 頁，縮放 125%」。
    //
    // 檢視區若不回報這三件事，UIA 只會把它當成一塊沒有內容的畫布——
    // 使用者按了 PageDown 之後不會得到任何回饋，也就無從知道翻頁成功了沒有。
    // 公開這個函式是為了讓測試能驗到與螢幕閱讀器完全相同的那一句話。
    [[nodiscard]] QString accessibleStatusText() const;

    // 目前頁在可視區裡的可見範圍，換算成該頁的頁內裝置座標（PRD-ZOOM-005）。
    // 頁面完全不在可視區內時回傳空矩形。
    [[nodiscard]] domain::RectI visibleRectInCurrentPage() const;

    // 參考線、格線與貼齊（PRD-VIEW-015 / PRD-VIEW-016）。
    // 判斷邏輯全在 domain/guides.h，這裡只負責畫出來與把拖曳點餵進去。
    [[nodiscard]] domain::GuideSet& guides() noexcept { return guides_; }
    void setGuidesVisible(bool visible);
    [[nodiscard]] bool guidesVisible() const noexcept { return guidesVisible_; }
    void setGrid(const domain::GridSettings& grid);
    [[nodiscard]] const domain::GridSettings& grid() const noexcept { return grid_; }
    void setSnapEnabled(bool enabled);

    // 表單欄位標示（PRD-FORM-004）。矩形由呼叫端提供——PageView 不認識表單，
    // 而欄位幾何來自 FormController 那條非同步路徑。這是純疊加，不改文件。
    void setFormFieldHighlight(bool enabled);
    [[nodiscard]] bool formFieldHighlight() const noexcept { return highlightFields_; }
    void setFormFieldRects(std::vector<std::pair<int, domain::RectF>> rects);
    [[nodiscard]] bool snapEnabled() const noexcept { return snapEnabled_; }

    // 目前頁的座標轉換，供尺規換算刻度。
    [[nodiscard]] domain::PageTransform currentPageTransform() const;
    // 目前頁左上角在檢視視窗座標系裡的位置（已扣掉捲動）。
    [[nodiscard]] QPoint currentPageOriginInViewport() const;

    // 把目前頁的某個頁內裝置座標捲到可視區左上角。Pan & Zoom 面板拖曳視框時用。
    void scrollToPointInCurrentPage(const domain::PointF& originInPage);

signals:
    void scaleChanged(double scale);
    void pageChanged(int pageIndex);
    // 可視區移動或改變大小。Pan & Zoom 面板據此更新視框位置——
    // 沒有這個訊號就只能輪詢，而輪詢在捲動時要嘛延遲要嘛吃 CPU。
    void viewportChanged();
    // 游標移到頁面上的哪個位置（頁面座標，點）。放大鏡面板據此取樣。
    // 只在游標真的落在某一頁上時發出——落在頁面之間的空白處時放大鏡
    // 應該維持上一個畫面，而不是顯示一塊灰底。
    void cursorMoved(int pageIndex, const alioth::domain::PointF& pagePoint);
    // 拖曳結束時發出，座標已換算成頁面空間。呈現層不決定要建立哪種註解，
    // 那是應用層的事——這裡只回報「使用者在這一頁框了這塊區域」。
    void shapeDrawn(int pageIndex, const alioth::domain::RectF& pageRect);
    // 框選完成（AreaSelect / Snapshot 工具）。與 shapeDrawn 分開發出，
    // 讓接收端不必再判斷「這個矩形是要畫註解還是要複製」。
    void areaSelected(int pageIndex, const alioth::domain::RectF& pageRect);
    // 多邊形／折線完成。closed 為真代表要自動封閉（Polygon）。
    void verticesDrawn(int pageIndex, const std::vector<alioth::domain::PointF>& vertices,
                       bool closed);
    // 使用者點了一個連結。呈現層不決定要不要跟隨——外部網址需要確認對話框
    // （PRD §8.2），那是應用層的判斷。
    void linkActivated(const alioth::domain::LinkTarget& target);
    // 選取工具在頁面上按下左鍵（且不是連結、不是三擊選行）。發出之後仍然照常
    // 進入選取流程——這是一個「順便告訴你使用者點了哪裡」的通知，不是一條
    // 會吃掉事件的分支。註釋視窗（PRD-ANN-004）用它判斷點到了哪一則註解；
    // 命中測試不放在這裡，因為 PageView 刻意不認識註解。
    void pageClicked(int pageIndex, const alioth::domain::PointF& pagePoint);
    // 觸控長按達到門檻時發出（PRD-UI-013：長按等同右鍵開選單）。呈現層只負責
    // 偵測手勢，選單內容交給持有 PageView 的容器決定——那需要知道命令匯流排、
    // 目前選取狀態等這個元件刻意不認識的東西。
    void longPressTriggered(int pageIndex, const alioth::domain::PointF& pagePoint,
                            const QPoint& widgetPoint);
    // 鉛筆筆畫完成（PRD-ANN-003）。帶壓力的原始取樣點，平滑與壓力分段
    // 都在應用層做——那是純函數，放在這裡會讓呈現層背上一套演算法。
    void strokeDrawn(int pageIndex, const std::vector<alioth::domain::PressurePoint>& points);
    // 文字標記類工具（螢光筆／底線／刪除線／波浪線）完成一次選取。
    //
    // 這些工具走的是選取路徑而不是拖曳矩形——標記要貼著字，不是畫一個方框。
    // 但選取本身不會產生註解，必須有人在放開滑鼠時說「就是現在」。
    // PageView 不認識標記種類，所以只回報「選好了」，由應用層決定寫哪一種。
    void markupSelectionCompleted();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // 數位板事件走獨立的通道（Qt 不會把它們合併進 mouse*Event），
    // 壓力值只在這裡拿得到。
    void tabletEvent(QTabletEvent* event) override;
    // 觸控事件送到 viewport，不是主 widget，所以覆寫這個而不是 event()——
    // QAbstractScrollArea 把兩者分開，觸控／手勢一律走 viewport（PRD-UI-013）。
    bool viewportEvent(QEvent* event) override;

private:
    void updateScrollRanges();
    void publishViewport();
    void syncLayoutSizes();
    // 以「頁面上的某一點維持在畫面同一位置」為條件套用新縮放（PRD-ZOOM-002）。
    void applyZoomAnchored(double newScale, const QPointF& anchorInWidget);
    [[nodiscard]] domain::RectI visibleDocumentRect() const;
    // 圖磚的渲染倍率＝顯示倍率 × 裝置像素比。版面與事件座標一律用顯示倍率。
    [[nodiscard]] double renderScale() const;
    // 命中測試連結。回傳 nullptr 表示該點沒有連結。
    [[nodiscard]] const domain::LinkTarget* linkAt(const PageHit& hit) const;

    void paintSelection(QPainter& painter, const domain::RectI& visible) const;
    void paintGuides(QPainter& painter, const domain::RectI& visible) const;
    void paintPendingVertices(QPainter& painter, const domain::RectI& visible) const;
    // 鉛筆的即時預覽。線寬跟著壓力變化，讓使用者當下就看得到力道的效果。
    void paintPendingStroke(QPainter& painter, const domain::RectI& visible) const;
    void paintFormFieldHighlights(QPainter& painter, const domain::RectI& visible) const;
    // 結束多邊形／折線的收集並發出訊號。commit 為 false 代表取消（Esc）。
    void finishVertexDrawing(bool commit);
    // 更新可及性描述並通知輔助技術。翻頁與縮放都要呼叫——
    // 只更新字串而不發事件，NVDA 與 JAWS 不會重新讀取，等於沒改。
    void announceState();

    // 依目前工具與 cursorSizeLevel_ 重新套用游標圖樣。系統游標（IBeam）不需要
    // 重畫，只有自畫的十字準星需要（見 setCursorSizeLevel 說明）。
    void updateToolCursor();
    // 觸控手勢處理，由 viewportEvent 呼叫。回傳 true 代表事件已被吃掉。
    bool handleTouchEvent(QEvent* event);

    app::DocumentController* controller_{nullptr};
    app::SelectionController* selection_{nullptr};
    QTimer* zoomSettleTimer_{nullptr};
    domain::LayoutOptions layoutOptions_{};
    domain::PageLayout layout_;
    std::int32_t pageIndex_{0};
    // 三擊選行的判定狀態。Qt 不送三擊事件，見 mouseDoubleClickEvent。
    QElapsedTimer sinceDoubleClick_;
    QPoint lastDoubleClickPos_;
    // 手形工具的拖曳起點（widget 座標）。
    QPoint panAnchor_;
    // 多邊形／折線正在收集的頂點。空代表沒有進行中的繪製。
    std::vector<domain::PointF> pendingVertices_;
    std::int32_t pendingVertexPage_{-1};
    domain::GuideSet guides_;
    domain::GridSettings grid_;
    bool guidesVisible_{false};
    bool snapEnabled_{false};
    double scale_{1.0};
    domain::Rotation rotation_{domain::Rotation::None};
    bool zooming_{false};
    bool dragging_{false};
    Tool tool_{Tool::Select};
    // 幾何註解拖曳中的起點與現況，兩者都是頁面座標。
    // 鉛筆：收集中的筆畫（頁面座標＋壓力）與它所在的頁。
    std::vector<domain::PressurePoint> pendingStroke_;
    std::int32_t pendingStrokePage_{-1};
    // 目前這一筆的壓力。QTabletEvent 會在 press/move 更新它；滑鼠事件維持 1.0。
    double currentPressure_{1.0};

    // 表單欄位標示：(頁碼, 頁面座標矩形)。
    bool highlightFields_{false};
    std::vector<std::pair<int, domain::RectF>> fieldRects_;

    domain::PointF shapeAnchor_{};
    domain::PointF shapeCurrent_{};
    std::int32_t shapePage_{-1};

    // 觸控最佳化（PRD-UI-013）。
    app::CursorSizeLevel cursorSizeLevel_{app::CursorSizeLevel::Normal};
    app::LongPressGesture longPress_;
    QTimer* longPressTimer_{nullptr};
    // 手勢的時間戳來源刻意用自己的 QElapsedTimer，不用 QTouchEvent::timestamp()：
    // 後者的紀元點因平台而異，混用會讓長按的時長判定在不同平台上不一致；
    // 自己算的單調時鐘保證 press/move/timer tick 用的是同一把尺。
    QElapsedTimer touchClock_;
    app::PinchZoomTracker pinchTracker_;
    bool pinchActive_{false};
};

}  // namespace alioth::ui
