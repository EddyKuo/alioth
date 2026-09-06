#include "engine/shellthumb/thumbnail_renderer.h"

#include <algorithm>
#include <cmath>

#include "engine/pdfium_library.h"
#include "fpdf_edit.h"
#include "fpdfview.h"

namespace alioth::engine::shellthumb {
namespace {

// RAII 包裝。這支程式碼跑在 explorer.exe 裡，任何一條提早返回的路徑漏掉
// 釋放，都會變成使用者「開了幾百個資料夾之後檔案總管越來越慢」。
struct DocumentHandle {
    FPDF_DOCUMENT value{nullptr};
    ~DocumentHandle() {
        if (value != nullptr) FPDF_CloseDocument(value);
    }
};

struct PageHandle {
    FPDF_PAGE value{nullptr};
    ~PageHandle() {
        if (value != nullptr) FPDF_ClosePage(value);
    }
};

struct BitmapHandle {
    FPDF_BITMAP value{nullptr};
    ~BitmapHandle() {
        if (value != nullptr) FPDFBitmap_Destroy(value);
    }
};

}  // namespace

ThumbnailResult renderFirstPageThumbnail(const std::string& path, int maxEdgePx) {
    ThumbnailResult result;
    const auto fail = [&result](std::string message) {
        result.ok = false;
        result.diagnostic = std::move(message);
        result.pixels.clear();
        result.width = 0;
        result.height = 0;
        return result;
    };

    if (path.empty()) return fail("路徑是空的");
    if (maxEdgePx <= 0 || maxEdgePx > 4096) {
        // 上限不是為了美觀：檔案總管要的縮圖最大也就幾百像素，
        // 而一個被要求產生 4 萬像素縮圖的 Shell 擴充會把記憶體吃光，
        // 症狀是整個檔案總管沒有回應。
        return fail("縮圖尺寸超出合理範圍");
    }

    PdfiumRuntime runtime;

    // 用 FPDF_LoadDocument 而不是 FPDF_LoadCustomDocument：主程式那邊用自訂
    // 讀取器是因為存檔要做原子更名（見 CLAUDE.md），而縮圖只讀不寫，
    // 沒有那個約束；少一層自訂回呼就少一條會在 explorer.exe 裡出錯的路徑。
    DocumentHandle document;
    document.value = FPDF_LoadDocument(path.c_str(), nullptr);
    if (document.value == nullptr) {
        // 加密文件也落在這裡。檔案總管的縮圖沒有互動餘地，不提示輸入密碼——
        // 會彈對話框的 Shell 擴充是使用者最痛恨的東西之一。
        return fail("無法開啟文件（可能損毀或有密碼保護）");
    }
    if (FPDF_GetPageCount(document.value) <= 0) return fail("文件沒有頁面");

    PageHandle page;
    page.value = FPDF_LoadPage(document.value, 0);
    if (page.value == nullptr) return fail("無法載入第一頁");

    const double widthPt = FPDF_GetPageWidth(page.value);
    const double heightPt = FPDF_GetPageHeight(page.value);
    if (!(widthPt > 0.0) || !(heightPt > 0.0)) return fail("頁面尺寸不合法");

    // 等比例縮放到最長邊。拉伸的縮圖在檔案總管的方格裡一眼就看得出來不對。
    const double scale = static_cast<double>(maxEdgePx) / std::max(widthPt, heightPt);
    const int width = std::max(1, static_cast<int>(std::lround(widthPt * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(heightPt * scale)));

    BitmapHandle bitmap;
    bitmap.value = FPDFBitmap_Create(width, height, 1);  // 1 = 帶 alpha
    if (bitmap.value == nullptr) return fail("無法配置點陣圖");

    // 先填白。PDF 的頁面背景在規格上是透明的，直接畫上去的話，
    // 深色主題的檔案總管會顯示一張看不見內容的圖。
    FPDFBitmap_FillRect(bitmap.value, 0, 0, width, height, 0xFFFFFFFF);
    // 整頁光柵化在這裡是允許的例外：離線、一次性、低解析度（見標頭說明）。
    // 不帶 FPDF_ANNOT——縮圖要呈現的是文件本身，別人的註解不該出現在
    // 檔案總管的預覽裡。
    FPDF_RenderPageBitmap(bitmap.value, page.value, 0, 0, width, height, 0, 0);

    const int stride = FPDFBitmap_GetStride(bitmap.value);
    const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap.value));
    if (buffer == nullptr || stride <= 0) return fail("點陣圖緩衝區不可用");

    // stride 不等於 width*4：PDFium 會做列對齊，假設相等在多數寬度下
    // 會產生斜切的畫面（與 CLAUDE.md 記的零複製 stride 陷阱同源）。
    result.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = buffer + static_cast<std::size_t>(y) *
                                               static_cast<std::size_t>(stride);
        std::copy(row, row + static_cast<std::size_t>(width) * 4u,
                  result.pixels.begin() + static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(width) * 4u);
    }

    result.width = width;
    result.height = height;
    result.ok = true;
    return result;
}

}  // namespace alioth::engine::shellthumb
