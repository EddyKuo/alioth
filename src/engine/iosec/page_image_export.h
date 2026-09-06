#pragma once

// 頁面匯出為點陣影像（PRD-IO-007）。
//
// 這是 CLAUDE.md「嚴禁整頁光柵化」的又一個合法例外，理由與
// engine/enhance/page_rasterizer.h 完全相同：使用者明確要求輸出一整張點陣圖，
// 輸出本身就沒有可視區的概念可言，離線、一次性、由使用者觸發。這裡直接重用
// renderPage()，不重新實作渲染，避免兩條路徑各自對 stride、旋轉、註解疊加
// 有不同的處理。
//
// 這一層是本工作包裡唯一需要 Qt 影像外掛（imageformats）的地方：PNG 走 Qt
// 內建（無需外掛），JPEG 與 TIFF 各自需要 qjpeg / qtiff 外掛存在於
// QT_PLUGIN_PATH 下。外掛缺席時不得寫出一個假造成功的空檔——一律在動筆之前
// 用 QImageWriter::supportedImageFormats() 查一次，缺就回報 UnsupportedFormat
// 並說明原因，讓呼叫端能明確降級（例如提示使用者改存 PNG）。

#include <cstdint>
#include <string>

namespace alioth::engine::iosec {

enum class ImageExportFormat { Png, Jpeg, Tiff };

enum class ImageExportStatus {
    Ok,
    InvalidPage,
    RenderFailed,
    UnsupportedFormat,  // 要求的格式沒有對應的 Qt 影像外掛
    WriteFailed,
};

[[nodiscard]] const char* describe(ImageExportStatus status) noexcept;
[[nodiscard]] const char* describe(ImageExportFormat format) noexcept;

struct ImageExportOptions {
    double dpi{150.0};
    ImageExportFormat format{ImageExportFormat::Png};
    // 匯出「看起來的樣子」而非「內容本身」，因此預設含註解——與列印
    // （PRD-IO-008）預設含註解的立場一致。使用者若只要內容，可另外關閉。
    bool includeAnnotations{true};
    int jpegQuality{90};  // 僅 Jpeg 格式使用，範圍 0–100
};

struct ImageExportResult {
    ImageExportStatus status{ImageExportStatus::InvalidPage};
    std::string message;
    std::int64_t bytesWritten{0};

    [[nodiscard]] bool ok() const noexcept { return status == ImageExportStatus::Ok; }
};

// 是否有對應的 Qt 影像外掛。呼叫端可以在顯示格式選單前先查一次，
// 把不支援的格式直接灰化，而不是讓使用者選了才失敗。
[[nodiscard]] bool isFormatSupported(ImageExportFormat format);

// 把單一頁面渲染並寫成獨立的影像檔。pdfBytes 是完整檔案內容
//（與 page_rasterizer 的其他入口一致，呼叫端負責把檔案讀進記憶體）。
[[nodiscard]] ImageExportResult exportPageImage(const std::string& pdfBytes,
                                                std::int32_t pageIndex,
                                                const std::string& outputPath,
                                                const ImageExportOptions& options);

}  // namespace alioth::engine::iosec
