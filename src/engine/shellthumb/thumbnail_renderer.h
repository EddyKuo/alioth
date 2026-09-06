#pragma once

// 檔案總管縮圖用的最小渲染器（PRD-UI-020）。
//
// 這一支存在的理由不是「又一個縮圖產生器」，而是**它會被載進 explorer.exe**。
// Shell 擴充是行內 COM 伺服器，作業系統把它載進檔案總管的行程裡，因此：
//
//   1. **不帶 Qt。** 主程式的縮圖路徑走 PdfiumEngine，而它經由平台層依賴
//      Qt Core。把 Qt 載進每一個開著資料夾的檔案總管視窗，是使用者不會
//      同意的代價。這裡直接用 PDFium，回傳原始像素，呼叫端自己包成 HBITMAP。
//   2. **同步、無執行緒。** PdfiumEngine 的非同步佇列在這裡是負擔：
//      Shell 擴充的合約就是「給我路徑，回我一張圖」，而且作業系統本來就會
//      在自己的工作執行緒上呼叫它。
//   3. **失敗必須安靜而完整。** 這支程式碼崩潰會把使用者的檔案總管一起帶走。
//      任何無法處理的輸入都回傳失敗，絕不丟例外、絕不斷言。
//
// 只渲染第一頁。整頁光柵化在這裡是允許的例外（與縮圖面板同一個理由：
// 離線、一次性、低解析度），理由寫在這裡而不是散在程式碼裡。

#include <cstdint>
#include <string>
#include <vector>

namespace alioth::engine::shellthumb {

struct ThumbnailResult {
    bool ok{false};
    std::string diagnostic;

    std::vector<std::uint8_t> pixels;  // BGRA，逐列由上而下
    int width{0};
    int height{0};
};

// 把 path 的第一頁渲染成最長邊不超過 maxEdgePx 的縮圖。
//
// 加密文件回傳失敗而不是提示輸入密碼：檔案總管的縮圖沒有互動的餘地，
// 而彈出對話框的 Shell 擴充是使用者最痛恨的東西之一。
[[nodiscard]] ThumbnailResult renderFirstPageThumbnail(const std::string& path, int maxEdgePx);

}  // namespace alioth::engine::shellthumb
