#include "engine/pdfium_library.h"

#include <fpdfview.h>

#include <mutex>

namespace alioth::engine {
namespace {

std::once_flag g_initOnce;

}  // namespace

PdfiumRuntime::PdfiumRuntime() {
    // 初始化一次，之後不再銷毀。
    //
    // 原本這裡是引用計數，最後一個持有者析構時呼叫 FPDF_DestroyLibrary。那是錯的：
    // PDFium 不保證 destroy 之後可以再 init，而子系統的生命週期是交錯的
    // （並行搜尋會反覆建立與銷毀多個擷取器）。實際症狀是計數歸零又回升之後，
    // 開檔開始無聲失敗——不是崩潰，是 FPDF_LoadCustomDocument 回 nullptr，
    // 看起來像檔案壞了。這個 bug 是並行搜尋的測試抓到的。
    //
    // 代價是行程結束時不呼叫 FPDF_DestroyLibrary。那個函式只釋放行程即將交還給
    // 作業系統的記憶體，不做任何外部可見的清理，因此不呼叫沒有實質損失。
    std::call_once(g_initOnce, [] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;  // 無 V8，恆為 nullptr
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    });
}

PdfiumRuntime::~PdfiumRuntime() = default;

}  // namespace alioth::engine
