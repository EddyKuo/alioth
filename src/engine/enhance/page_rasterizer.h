#pragma once

// 頁面點陣化（WBS 14，PRD-ENH-003）。
//
// 這裡是 CLAUDE.md「嚴禁整頁光柵化」的合法例外之一，比照縮圖與「裁切至白邊」
// 的先例：那條規則的目的是控制**互動式檢視**的記憶體與延遲（可視區只需要
// 幾張 512×512 圖磚），而點陣化的輸出本來就是一整張圖——切成圖磚再拼回去
// 只會多一次全圖複製與拼接處的取樣誤差。這條路徑同樣是離線、一次性、
// 由使用者明確觸發的，不在任何渲染迴圈上。
//
// 另外兩件必須寫明的事：
//
//   1. 渲染時**不帶 FPDF_ANNOT**。點陣化的是內容不是標記；把註解一起烤進圖裡，
//      再加上它們仍然存在於 /Annots，結果是每個標記出現兩次。
//   2. /Rotate 不為零的頁面，PDFium 回傳的是「顯示後」的點陣圖，而內容串流
//      畫在使用者空間。差這一步的症狀是點陣化之後整頁躺平 90 度。
//      這裡的處置是把點陣圖轉回使用者空間的方向，/Rotate 保持不動。
//
// PDFium 非執行緒安全：本檔以自己的文件把手同步完成，呼叫端不得與其他
// PDFium 子系統同時對同一份位元組動作。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "domain/geometry.h"
#include "engine/enhance/image_codec.h"
#include "engine/objects/incremental_appender.h"
#include "engine/pixel_buffer.h"

namespace alioth::engine::enhance {

struct RasterizedPage {
    bool ok{false};
    std::string diagnostic{};
    PixelBuffer pixels{};      // 已轉回使用者空間方向
    domain::RectF box{};       // 頁面的可見矩形（點）
};

// 把單頁渲染成點陣圖。測試也用它取像素驗證寫入結果。
[[nodiscard]] RasterizedPage renderPage(const std::string& pdfBytes, std::int32_t pageIndex,
                                        double dpi, bool includeAnnotations = false);

// 文件快照工具（PRD-TXT-005）：使用者在檢視器上框一塊區域，複製為點陣影像。
//
// 這是 CLAUDE.md「嚴禁整頁光柵化」的第三個合法例外，與縮圖、裁切至白邊並列：
// 三者的共通點是離線、一次性、由使用者明確觸發，不在任何互動渲染迴圈上。
// 但快照與另外兩者不同的地方是**只渲染框選的那一塊**，不是整頁——
// 這不只是省記憶體，是因為「使用者只要那一小塊」本身就是這個工具存在的理由，
// 給他一整頁圖裁一角，會在大型工程圖上把記憶體成本推回整頁光柵化的等級，
// 違背了規則存在的目的。
//
// 座標系選擇：用「頁面顯示空間」（原點左上、Y 向下、單位點）而不是領域層慣用的
// 頁面空間（原點左下、Y 向上），因為這個函式直接對應 FPDF_GetPageWidthF /
// FPDF_GetPageHeightF 描述的「顯示後」座標——與 renderPage 用的是同一個參考系。
// 刻意不重用 domain::RectF：那個型別的慣例是 Y 向上，混用兩種 Y 方向卻共用同一個
// 型別，正是 CLAUDE.md 座標系一節警告過的「看起來對、實際上下顛倒」錯誤的來源。
struct SnapshotArea {
    double left{0.0};
    double top{0.0};
    double right{0.0};
    double bottom{0.0};

    [[nodiscard]] double width() const noexcept { return right - left; }
    [[nodiscard]] double height() const noexcept { return bottom - top; }
    [[nodiscard]] bool isEmpty() const noexcept { return width() <= 0.0 || height() <= 0.0; }
};

struct SnapshotResult {
    bool ok{false};
    std::string diagnostic{};
    PixelBuffer pixels{};
    SnapshotArea clippedArea{};  // 與頁面邊界相交、正規化後實際擷取到的範圍
};

// 把 area（顯示空間，點）裁切渲染成點陣圖，只配置該區域大小的緩衝區。
//
// 實作上仍然呼叫 FPDF_RenderPageBitmap 讓整頁走一遍它的預設矩陣（沿用 renderPage
// 已驗證正確的 /Rotate 處理），但把輸出點陣圖限制在框選範圍大小，並以負的
// start_x/start_y 把整頁位圖「平移」到只有框選範圍落在緩衝區內——PDFium 會自動
// 裁掉緩衝區外的部分。PDFium 仍會在內部把整份內容串流解析一次（這是向量渲染
// 本質決定的，裁切範圍改變不了這一步的成本），但**輸出緩衝區與逐像素寫入成本**
// 被限制在框選範圍，這正是「嚴禁整頁光柵化」規則實際要控制的東西
// （見 CLAUDE.md「圖磚化」段落：記憶體預算來自可視區，不是文件大小）。
[[nodiscard]] SnapshotResult renderSnapshot(const std::string& pdfBytes, std::int32_t pageIndex,
                                            const SnapshotArea& area, double dpi,
                                            bool includeAnnotations = true);

// 把一張影像寫成頁面的唯一內容：登記 XObject 資源、產生繪製指令、取代 /Contents。
// 點陣化與掃描增強做的是同一件事（差別只在影像從哪來），共用同一份實作，
// 避免其中一條路徑日後單獨修正了資源繼承或 /Contents 形態的處理。
[[nodiscard]] bool writePageAsImage(objects::IncrementalAppender& appender,
                                    const objects::PdfRef& page, const domain::RectF& box,
                                    const EncodedImage& image, const std::string& resourceName,
                                    std::string* diagnostic);

// 把 PDFium 的渲染結果轉回使用者空間的方向（/Rotate 的反向直角旋轉）。
// 直角旋轉是像素重排，完全無損，因此可以放心在寫回之前做。
[[nodiscard]] PixelBuffer orientToUserSpace(const objects::PdfSourceDocument& source,
                                            const objects::PdfRef& page,
                                            const PixelBuffer& rendered);

struct RasterizeResult {
    bool ok{false};
    std::string diagnostic{};
    std::vector<std::int32_t> pagesChanged{};
    std::int64_t imageBytes{0};
};

// 把指定頁面（空代表全部）的內容換成單一影像。
//
// 渲染的來源是 appender **開啟時**的位元組：PDFium 讀的是檔案，看不到還沒
// build() 出來的附加段。因此「先加背景再點陣化」必須分兩次做，中間把位元組
// 產生出來——這個限制寫在這裡，免得日後串成一條管線時得到一張少了背景的圖。
[[nodiscard]] RasterizeResult rasterizePages(objects::IncrementalAppender& appender,
                                             const std::vector<std::int32_t>& pages,
                                             const domain::enhance::RasterizeSettings& settings);

}  // namespace alioth::engine::enhance
