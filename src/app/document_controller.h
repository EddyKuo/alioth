#pragma once

// DocumentController — 呈現層與引擎之間的唯一橋樑。
//
// 負責三件事：
//   1. 把引擎執行緒送回的結果編組到 GUI 執行緒（Qt 排隊式連線）
//   2. 依可視區算出該渲染哪些圖磚、哪些只是預取
//   3. 可視區一變就取消過期任務（PRD-VIEW-003，16 毫秒內）
//
// 呈現層不得直接持有 PdfiumEngine。

#include <QColor>
#include <QObject>
#include <QImage>
#include <QString>

#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/save/autosave.h"

#include "domain/document.h"
#include "domain/geometry.h"
#include "domain/ocg.h"
#include "domain/tile.h"
#include "engine/cancellation.h"
#include "engine/pdfium_engine.h"
#include "engine/tile_cache.h"

namespace alioth::app {

// 一頁需要渲染的範圍，座標是頁內裝置空間（原點頁面左上）。
//
// 版面計算刻意留在呈現層而不是控制器：Split View（PRD-VIEW-017）要求兩個檢視
// 各自獨立縮放與捲動，而版面是縮放的函數。版面若歸控制器所有，兩個檢視就得
// 共用同一份版面，Split View 就永遠做不出來。
//
// 控制器只負責它真正該負責的事：排程、快取、跨執行緒編組。
struct PageTileRequest {
    std::int32_t pageIndex{0};
    domain::RectI visibleInPage{};  // 可視區裁進該頁之後的範圍
    domain::RectI pageSize{};       // 該頁在目前倍率下的完整像素尺寸
};

// 一次排程的整體參數。
struct TileScheduleOptions {
    double scale{1.0};
    domain::Rotation rotation{domain::Rotation::None};
    // 連續縮放期間只排可見圖磚。每一格中間倍率都排一輪預取，等於把佇列灌滿
    // 註定要被丟掉的工作，反而拖慢使用者真正在看的那幾塊。
    bool zooming{false};
};

class DocumentController : public QObject {
    Q_OBJECT

public:
    explicit DocumentController(QObject* parent = nullptr);
    ~DocumentController() override;

    void openDocument(const QString& path, const QString& password = {});
    void closeDocument();

    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] const domain::DocumentInfo& info() const noexcept { return info_; }
    [[nodiscard]] std::int32_t pageCount() const noexcept { return info_.pageCount; }
    [[nodiscard]] domain::SizeF pageSizePt(std::int32_t index) const;
    // 真實尺寸是否已經問到。false 代表 pageSizePt() 現在回的是 A4 佔位值，
    // 呼叫端若要做「對齊實際紙張」之類的決定，必須先等它變 true。
    [[nodiscard]] bool pageGeometryKnown(std::int32_t index) const;

    // 按需補載頁面尺寸。開檔時只先問前 kInitialGeometryPages 頁——一次問一萬頁
    // 會塞爆那條唯一的 PDFium 執行緒，而版面只需要看得到的那幾頁。
    // 範圍會自行裁到合法頁碼，已問過的頁不會重複送出。
    void ensurePageGeometry(std::int32_t fromPage, std::int32_t toPage);

    // 排程渲染。呼叫端（呈現層）算好哪些頁的哪些範圍看得到，控制器不反推版面。
    // 每次呼叫都會取消上一批尚未開始的預取任務。
    void scheduleTiles(const std::vector<PageTileRequest>& pages,
                       const TileScheduleOptions& options);

    // 取快取中的圖磚。回傳的 QImage 包裝的是引擎緩衝區本體，不複製像素；
    // 內含的 shared_ptr 由清理函式持有，確保 QImage 存活期間記憶體不被回收。
    // 沒有命中就回傳 null image，並已排程渲染。
    [[nodiscard]] QImage tileIfReady(const domain::TileKey& key);

    // 書籤與縮圖都是背景工作，結果以訊號送出，呼叫端不必等待。
    void requestOutline();
    // 連結（PRD-NAV-006）。頁面載入後連結不會變，所以整頁一次取回並快取，
    // 而不是滑鼠每動一格就問一次引擎。
    void requestLinks(std::int32_t pageIndex);
    [[nodiscard]] const std::vector<domain::LinkTarget>& linksForPage(std::int32_t pageIndex) const;
    // 註解列表（PRD-ANN-008）。只掃目前可見的頁面範圍，
    // 一次掃一萬頁在大型文件上等於凍結介面。
    void requestAnnotations(std::int32_t fromPage, std::int32_t toPage);
    void requestThumbnail(std::int32_t pageIndex, std::int32_t maxEdgePixels = 160);

    // 圖層（OCG）面板（PRD-VIEW-008）。解析在背景執行緒進行，且刻意不碰
    // PDFium 的文件把手——/OCProperties 是我們自己的物件剖析器讀的（見
    // engine/layers/ocg_reader.h），不佔用僅有的那一條 PDFium 執行緒。
    //
    // 已知邊界：勾選只更新 layers() 回傳的模型與面板 UI，*不會*觸發重新渲染。
    // PDFium 目前對外的公開 API 沒有執行期 OC 狀態的入口，見 domain/ocg.h
    // 開頭的說明與 exceptions/EXC_20260906_RD_SA_ocg_render_gap.md。
    void requestLayers();
    [[nodiscard]] const domain::OcgTree& layers() const noexcept { return layers_; }
    // 回傳實際改動可見性的節點（含互斥群組連動），供呼叫端局部刷新面板。
    std::vector<std::int32_t> setLayerVisible(std::int32_t objectNumber, bool visible);

    // 自動儲存（PRD-IO-003）。引擎層刻意不擁有事件迴圈，所以計時器在這裡，
    // 由 QTimer 定期問 AutosaveManager 是否到期。
    void setDocumentDirty();
    void runAutosaveIfDue();

    // 偏好設定的套用點。控制器刻意不自己去讀 Settings：那會讓「設定何時生效」
    // 變成兩個地方的競賽，而且測試就得準備一份 QSettings。
    void setCacheBytes(std::size_t bytes);
    void setAutosaveSeconds(int seconds);

    void setNightMode(bool enabled);
    void setRenderQuality(bool grayscale, bool smoothPaths, bool smoothText, bool smoothImages);
    [[nodiscard]] const engine::RenderOptions& renderOptions() const noexcept { return renderOptions_; }
    // 自訂背景與文字色（PRD-VIEW-007）。與夜間模式互斥，夜間模式優先。
    void setCustomColors(bool enabled, const QColor& background, const QColor& text);
    [[nodiscard]] bool nightMode() const noexcept { return renderOptions_.nightMode; }

    // 透明度格線（PRD-VIEW-018）。純呈現層概念，但渲染緩衝區怎麼初始化
    // 是引擎轉接層的事（見 engine::RenderOptions::transparencyGrid），
    // 因此一樣走 renderOptions_ 這條路，切換時清快取的理由與夜間模式相同：
    // 選項不是圖磚鍵的一部分，不清快取就會繼續顯示切換前烘進去的舊圖磚。
    void setTransparencyGrid(bool enabled);
    [[nodiscard]] bool transparencyGrid() const noexcept {
        return renderOptions_.transparencyGrid;
    }

    [[nodiscard]] engine::CacheStats cacheStats() const { return cache_.stats(); }
    [[nodiscard]] const std::vector<domain::OutlineNode>& outline() const noexcept {
        return outline_;
    }
    [[nodiscard]] const std::vector<domain::AnnotationSummary>& annotations() const noexcept {
        return annotations_;
    }

signals:
    void documentOpened(const QString& path);
    void documentOpenFailed(int error, const QString& message);
    void documentClosed();
    void autosaved(const QString& path);
    void autosaveFailed(const QString& message);
    void tileReady(alioth::domain::TileKey key);
    void outlineReady();
    void linksReady(int pageIndex);
    void annotationsReady();
    void thumbnailReady(int pageIndex, const QImage& image);
    void pageGeometryChanged();
    void layersReady();
    void layerVisibilityChanged();

private:
    void requestTile(const domain::TileKey& key, domain::TaskPriority priority);
    void loadPageGeometry();

    std::unique_ptr<engine::PdfiumEngine> engine_;
    engine::TileCache cache_;
    engine::CancellationSource viewportGeneration_;
    engine::RenderOptions renderOptions_{};
    std::uint64_t renderGeneration_{0};  // Accessed only on the controller's thread.

    // 換文件時遞增。圖磚用 renderGeneration_（顯示選項一改也要作廢），
    // 頁面幾何只在換文件時作廢——夜間模式不會讓 A4 變成 A3。
    std::uint64_t documentGeneration_{0};

    domain::DocumentInfo info_{};
    std::vector<domain::SizeF> pageSizes_;
    // 已送出 pageInfo 請求的頁。沒有這張表的話，每次捲動都會對同一批頁面
    // 重送請求，而那條 PDFium 執行緒是全行程唯一的一條。
    std::vector<char> geometryRequested_;
    engine::save::AutosaveManager autosave_;
    class QTimer* autosaveTimer_{nullptr};
    TileScheduleOptions lastOptions_{};
    QString path_;
    std::vector<domain::OutlineNode> outline_;
    std::vector<domain::AnnotationSummary> annotations_;
    std::unordered_map<std::int32_t, std::vector<domain::LinkTarget>> links_;
    domain::OcgTree layers_;
    // 圖層解析的背景執行緒。刻意不 detach：detach 後若 DocumentController
    // 在執行緒完成前被摧毀，執行緒結束時呼叫 QMetaObject::invokeMethod(this, ...)
    // 會踩到已釋放的記憶體。解構時明確 join，與 engine_.reset() 的理由一致
    // （docs/SDD.md §3.2）。
    std::thread layerThread_;
    bool open_{false};
};

}  // namespace alioth::app

Q_DECLARE_METATYPE(alioth::domain::TileKey)
