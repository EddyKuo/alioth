#include "engine/iosec/page_image_export.h"

#include <QByteArray>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QString>

#include "engine/enhance/page_rasterizer.h"

namespace alioth::engine::iosec {
namespace {

QByteArray qtFormatName(ImageExportFormat format) {
    switch (format) {
        case ImageExportFormat::Png:  return QByteArrayLiteral("PNG");
        case ImageExportFormat::Jpeg: return QByteArrayLiteral("JPG");
        case ImageExportFormat::Tiff: return QByteArrayLiteral("TIFF");
    }
    return {};
}

// PixelBuffer 是 FPDFBitmap_BGRA 直接寫入的記憶體：小端序下 BGRA 四個位元組
// 與 QImage::Format_ARGB32 的記憶體佈局相同（0xAARRGGBB 在小端序即
// B,G,R,A），因此可以直接包裝而不需要逐像素轉換——這與
// engine/enhance/image_codec.cpp 的 wrapPixels() 是同一個假設，兩處合起來
// 才是完整的證據：image_codec 的測試已經逐像素驗證過這個假設成立。
//
// stride 一律顯式帶入（CLAUDE.md 硬性限制 3），並在返回前用 QImage::copy()
// 深拷貝一份——PixelBuffer 在函式結束時就會被釋放，QImage 不能繼續指著
// 一塊已經還給系統的記憶體。
QImage wrapAndOwn(const engine::PixelBuffer& pixels) {
    if (pixels.isNull()) return {};
    const QImage view(pixels.data(), pixels.width(), pixels.height(),
                      static_cast<qsizetype>(pixels.stride()), QImage::Format_ARGB32);
    return view.copy();
}

}  // namespace

const char* describe(ImageExportStatus status) noexcept {
    switch (status) {
        case ImageExportStatus::Ok:                return "成功";
        case ImageExportStatus::InvalidPage:       return "頁碼超出範圍";
        case ImageExportStatus::RenderFailed:      return "渲染失敗";
        case ImageExportStatus::UnsupportedFormat: return "此格式沒有對應的 Qt 影像外掛";
        case ImageExportStatus::WriteFailed:       return "寫檔失敗";
    }
    return "未知狀態";
}

const char* describe(ImageExportFormat format) noexcept {
    switch (format) {
        case ImageExportFormat::Png:  return "PNG";
        case ImageExportFormat::Jpeg: return "JPEG";
        case ImageExportFormat::Tiff: return "TIFF";
    }
    return "未知格式";
}

bool isFormatSupported(ImageExportFormat format) {
    const QByteArray name = qtFormatName(format);
    if (name.isEmpty()) return false;
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    for (const QByteArray& entry : supported) {
        if (entry.compare(name, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

ImageExportResult exportPageImage(const std::string& pdfBytes, std::int32_t pageIndex,
                                  const std::string& outputPath,
                                  const ImageExportOptions& options) {
    ImageExportResult result;

    if (pageIndex < 0) {
        result.status = ImageExportStatus::InvalidPage;
        result.message = "頁碼不可為負值";
        return result;
    }

    // 格式支援與否在渲染之前就先查：渲染一張高解析度大圖再發現寫不出去，
    // 是白白浪費使用者的等待時間。
    if (!isFormatSupported(options.format)) {
        result.status = ImageExportStatus::UnsupportedFormat;
        result.message = std::string("找不到 ") + describe(options.format) +
                         " 的 Qt 影像外掛（imageformats），已改為不寫出任何檔案。"
                         "確認 QT_PLUGIN_PATH 是否含 imageformats 目錄。";
        return result;
    }

    const enhance::RasterizedPage rasterized =
        enhance::renderPage(pdfBytes, pageIndex, options.dpi, options.includeAnnotations);
    if (!rasterized.ok) {
        result.status = ImageExportStatus::RenderFailed;
        result.message = rasterized.diagnostic.empty() ? "渲染失敗" : rasterized.diagnostic;
        return result;
    }

    QImage image = wrapAndOwn(rasterized.pixels);
    if (image.isNull()) {
        result.status = ImageExportStatus::RenderFailed;
        result.message = "渲染結果為空影像";
        return result;
    }

    // 兩種讀者：PNG/TIFF 讀 dotsPerMeter 換算 DPI，JPEG 的 qjpeg 外掛同樣依此
    // 寫出 JFIF 密度欄位。統一在這裡設，不必對每種格式各寫一次換算。
    const int dotsPerMeter = static_cast<int>(options.dpi / 0.0254 + 0.5);
    image.setDotsPerMeterX(dotsPerMeter);
    image.setDotsPerMeterY(dotsPerMeter);

    QImageWriter writer(QString::fromStdString(outputPath), qtFormatName(options.format));
    if (options.format == ImageExportFormat::Jpeg) {
        writer.setQuality(options.jpegQuality);
    }
    if (!writer.write(image)) {
        result.status = ImageExportStatus::WriteFailed;
        result.message = writer.errorString().isEmpty()
                             ? "寫檔失敗"
                             : writer.errorString().toStdString();
        return result;
    }

    result.status = ImageExportStatus::Ok;
    // 實際落盤的檔案大小，不是未壓縮像素大小——後者對 PNG/JPEG 完全沒有參考意義。
    result.bytesWritten = QFileInfo(QString::fromStdString(outputPath)).size();
    result.message = "已匯出";
    return result;
}

}  // namespace alioth::engine::iosec
