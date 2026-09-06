#pragma once

// PdfiumEngine — 引擎轉接層的門面，也是全專案唯一允許呼叫 PDFium 的地方。
//
// PDFium 非執行緒安全：同一份文件把手在同一時間只能有一個執行緒觸碰。
// 因此本類別持有一條專用執行緒，所有 PDFium 呼叫都在該執行緒上序列化執行；
// 呼叫端只透過工作佇列與它溝通。禁止把 PDFium 呼叫搬到執行緒池。
//
// 需要真正並行（縮圖、搜尋）時的正確做法不是多執行緒共用把手，
// 而是另外開一個 PdfiumEngine 實體，各自持有同一檔案的獨立文件把手。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/annotation.h"
#include "domain/document.h"
#include "domain/geometry.h"
#include "domain/tile.h"
#include "engine/cancellation.h"
#include "engine/pixel_buffer.h"

namespace alioth::engine {

struct OpenResult {
    domain::DocumentError error{domain::DocumentError::None};
    domain::DocumentInfo info{};

    [[nodiscard]] bool ok() const noexcept { return error == domain::DocumentError::None; }
};

struct RenderResult {
    domain::TileKey key{};
    PixelBufferPtr buffer;
    bool cancelled{false};

    [[nodiscard]] bool ok() const noexcept { return buffer != nullptr && !cancelled; }
};

struct RenderOptions {
    bool drawAnnotations{true};   // FPDF_ANNOT
    bool lcdText{false};          // FPDF_LCD_TEXT，子像素抗鋸齒
    bool nightMode{false};        // PRD-VIEW-007

    // 自訂背景與文字色（PRD-VIEW-007 的另一半）。
    //
    // 與 nightMode 分開而不是把它做成「其中一組配色」：夜間模式是整幅反相，
    // 影像也跟著變負片；自訂配色只換「接近白的底」與「接近黑的字」，
    // 照片與圖表維持原色。使用者要的是「白底刺眼」的解法，不是把照片變負片。
    //
    // 只有兩端會被替換，中間的灰階依原始亮度在兩色之間內插——直接二值化會讓
    // 抗鋸齒的字緣變成鋸齒。
    bool customColors{false};
    std::uint8_t backgroundR{255}, backgroundG{255}, backgroundB{255};
    std::uint8_t textR{0}, textG{0}, textB{0};
    bool thinLines{false};        // PRD-VIEW-010

    // Stroke Adjust（PRD-VIEW-013）。PDF 的 stroke adjustment 是把細線對齊像素格，
    // PDFium 沒有對應旗標；最接近的是關掉路徑抗鋸齒（FPDF_RENDER_NO_SMOOTHPATH），
    // 效果同樣是細線變銳利而不是糊成灰帶。這是近似而不是等價，工程圖上
    // 0.1 pt 的線在低倍率下仍可能整條消失——那是取樣的問題，不是抗鋸齒的問題。
    bool strokeAdjust{false};

    // 關閉文字與影像的平滑化。列印預覽與像素比對時要用：抗鋸齒會讓
    // 同一份內容在不同機器上產生不同的邊緣像素，黃金影像比對因此不穩定。
    bool smoothText{true};
    bool smoothImages{true};

    bool grayscale{false};  // FPDF_GRAYSCALE，灰階預覽

    // 透明度格線（PRD-VIEW-018）。開啟時渲染緩衝區改以透明（alpha = 0）起始，
    // 而不是預設的不透明白底：PDF 頁面本身沒有畫到的地方會保留 alpha = 0，
    // 讓呈現層可以在圖磚底下畫棋盤格，透過真正透明的區域看見格線。
    // 這是唯一會影響「怎麼初始化緩衝區」而非「傳給 PDFium 哪個旗標」的選項。
    bool transparencyGrid{false};
};

// 渲染完成回呼在引擎執行緒上被呼叫。呼叫端負責馬上把結果排回自己的執行緒，
// 不得在回呼裡做繁重工作——那會卡住整條渲染佇列。
using RenderCallback = std::function<void(RenderResult)>;

class PdfiumEngine {
public:
    PdfiumEngine();
    ~PdfiumEngine();

    PdfiumEngine(const PdfiumEngine&) = delete;
    PdfiumEngine& operator=(const PdfiumEngine&) = delete;

    // 以下皆為非同步：把工作排入佇列後立刻返回，結果經回呼送出。
    void openDocument(std::string path, std::string password,
                      std::function<void(OpenResult)> callback);
    void closeDocument(std::function<void()> callback = {});

    void renderTile(domain::TileKey key, RenderOptions options,
                    domain::TaskPriority priority, CancellationToken token,
                    RenderCallback callback);

    void pageInfo(std::int32_t pageIndex, std::function<void(std::optional<domain::PageInfo>)> callback);

    // 列舉某一頁的註解摘要（PRD-ANN-008）。註解讀取與渲染共用同一份文件把手，
    // 所以必須排在同一條執行緒的佇列上。
    void pageAnnotations(std::int32_t pageIndex,
                         std::function<void(std::vector<domain::AnnotationSummary>)> callback);

    // 某一頁的所有連結（PRD-NAV-006）。一次取整頁而不是逐點查詢：
    // 滑鼠移動時每一格都問一次引擎會把佇列灌滿，而連結在頁面載入後不會變。
    void pageLinks(std::int32_t pageIndex,
                   std::function<void(std::vector<domain::LinkTarget>)> callback);

    // 書籤樹（PRD-NAV-003）。巢狀深度可達 8 層以上，解析在引擎執行緒上一次做完，
    // 因為 PDFium 的書籤走訪 API 只能在持有文件把手的執行緒上呼叫。
    void outline(std::function<void(std::vector<domain::OutlineNode>)> callback);

    // 縮圖（PRD-NAV-004）。縮圖是唯一允許整頁光柵化的路徑——它本來就是整頁的縮小版，
    // 且以最低優先權排程，不得影響主視圖幀率。
    void renderThumbnail(std::int32_t pageIndex, std::int32_t maxEdgePixels,
                         CancellationToken token, RenderCallback callback);

    // 在引擎執行緒上對目前開啟的文件執行工作，把手以 void* 傳出（實際型別 FPDF_DOCUMENT）。
    //
    // 存在理由是自動儲存與存檔：那些子系統需要文件把手，但把手只能在這條執行緒上使用。
    // 傳 void* 而不是 FPDF_DOCUMENT，是為了讓 PDFium 標頭不外洩到引擎轉接層以外；
    // 呼叫端只能把它轉交給同屬引擎層的元件，不該自行解讀。
    // 文件未開啟時以 nullptr 呼叫，呼叫端必須處理。
    void withDocument(std::function<void(void* document)> work);

    // 丟棄佇列中尚未開始、且優先權不高於指定等級的任務。
    // 捲動與縮放時呼叫，避免佇列被過期任務塞爆。
    void discardPending(domain::TaskPriority atOrBelow);

    // 同步等待佇列清空，僅供測試使用。
    void waitForIdle();

    [[nodiscard]] std::size_t pendingTaskCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine
