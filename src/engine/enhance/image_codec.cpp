#include "engine/enhance/image_codec.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include <algorithm>
#include <cstring>

namespace alioth::engine::enhance {
namespace {

using domain::enhance::CompressionSettings;
using domain::enhance::ImageCodec;

// QImage 包裝 PixelBuffer 時 stride 一律顯式傳入。BGRA 下 stride 通常等於
// 寬 × 4，但 Qt 對自己配置的緩衝區會做四位元組對齊，一旦兩者不等，
// 錯誤的 stride 產生的是斜切畫面而不是崩潰——測試極難察覺（SDD §4.1）。
[[nodiscard]] QImage wrapPixels(const PixelBuffer& pixels) {
    if (pixels.isNull()) return {};
    return QImage(pixels.data(), pixels.width(), pixels.height(),
                  static_cast<qsizetype>(pixels.stride()), QImage::Format_ARGB32);
}

[[nodiscard]] PixelBuffer fromQImage(const QImage& image) {
    if (image.isNull()) return {};
    const QImage converted = image.convertToFormat(QImage::Format_ARGB32);
    PixelBuffer out(converted.width(), converted.height());
    for (int y = 0; y < converted.height(); ++y) {
        std::memcpy(out.scanline(y), converted.constScanLine(y),
                    std::min<std::size_t>(out.stride(),
                                          static_cast<std::size_t>(converted.bytesPerLine())));
    }
    return out;
}

// qCompress 的輸出前面有四個位元組的原始長度，那不是 zlib 資料的一部分。
// 連著寫進 PDF 的話，解析器會在 FlateDecode 的第一個位元組就失敗，
// 而多數檢視器會靜默顯示空白——不是錯誤訊息。
[[nodiscard]] std::string zlibDeflate(const std::string& input) {
    const QByteArray compressed =
        qCompress(reinterpret_cast<const uchar*>(input.data()),
                  static_cast<qsizetype>(input.size()), 9);
    if (compressed.size() <= 4) return {};
    return std::string(compressed.constData() + 4,
                       static_cast<std::size_t>(compressed.size() - 4));
}

[[nodiscard]] bool isGrayscale(const PixelBuffer& pixels) {
    for (std::int32_t y = 0; y < pixels.height(); ++y) {
        const std::uint8_t* row = pixels.data() + pixels.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < pixels.width(); ++x) {
            if (row[x * 4 + 0] != row[x * 4 + 1] || row[x * 4 + 1] != row[x * 4 + 2]) return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasTransparency(const PixelBuffer& pixels) {
    for (std::int32_t y = 0; y < pixels.height(); ++y) {
        const std::uint8_t* row = pixels.data() + pixels.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < pixels.width(); ++x) {
            if (row[x * 4 + 3] != 255) return true;
        }
    }
    return false;
}

// alpha 通道 → /SMask 的灰階取樣。遮罩一律無損：把 alpha 編成 JPEG
// 會在邊緣產生半透明的振鈴，表現為物件周圍的一圈灰邊。
[[nodiscard]] std::string encodeSoftMask(const PixelBuffer& pixels) {
    std::string samples;
    samples.reserve(static_cast<std::size_t>(pixels.width()) *
                    static_cast<std::size_t>(pixels.height()));
    for (std::int32_t y = 0; y < pixels.height(); ++y) {
        const std::uint8_t* row = pixels.data() + pixels.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < pixels.width(); ++x) {
            samples.push_back(static_cast<char>(row[x * 4 + 3]));
        }
    }
    return zlibDeflate(samples);
}

}  // namespace

DecodedImage decodeImageFile(const std::string& bytes) {
    DecodedImage result;
    QImage image;
    if (!image.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                            static_cast<int>(bytes.size()))) {
        result.diagnostic = "無法辨識的影像格式";
        return result;
    }
    result.hasAlpha = image.hasAlphaChannel();
    result.pixels = fromQImage(image);
    result.ok = !result.pixels.isNull();
    if (!result.ok) result.diagnostic = "影像解碼後為空";
    return result;
}

DecodedImage decodeJpeg(const std::string& bytes) { return decodeImageFile(bytes); }

DecodedImage decodeRawSamples(const std::string& samples, std::int32_t width, std::int32_t height,
                              std::int32_t bitsPerComponent, const std::string& colorSpace) {
    DecodedImage result;
    if (width <= 0 || height <= 0) {
        result.diagnostic = "影像尺寸不合法";
        return result;
    }
    if (bitsPerComponent != 8) {
        result.diagnostic = "只支援 8 位元取樣";
        return result;
    }

    std::int32_t components = 0;
    if (colorSpace == "DeviceGray" || colorSpace == "CalGray" || colorSpace == "G") {
        components = 1;
    } else if (colorSpace == "DeviceRGB" || colorSpace == "CalRGB" || colorSpace == "RGB") {
        components = 3;
    } else {
        result.diagnostic = "不支援的色彩空間：" + colorSpace;
        return result;
    }

    const auto expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                          static_cast<std::size_t>(components);
    if (samples.size() < expected) {
        result.diagnostic = "取樣資料長度不足";
        return result;
    }

    PixelBuffer pixels(width, height);
    const auto* src = reinterpret_cast<const std::uint8_t*>(samples.data());
    for (std::int32_t y = 0; y < height; ++y) {
        std::uint8_t* dst = pixels.scanline(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)) * static_cast<std::size_t>(components);
            const std::uint8_t r = src[offset];
            const std::uint8_t g = components == 1 ? r : src[offset + 1];
            const std::uint8_t b = components == 1 ? r : src[offset + 2];
            dst[x * 4 + 0] = b;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = r;
            dst[x * 4 + 3] = 255;
        }
    }
    result.pixels = std::move(pixels);
    result.ok = true;
    return result;
}

EncodedImage encodeJpeg(const PixelBuffer& pixels, std::int32_t quality) {
    EncodedImage result;
    if (pixels.isNull()) {
        result.diagnostic = "來源影像為空";
        return result;
    }

    const QImage image = wrapPixels(pixels);
    QByteArray buffer;
    QBuffer device(&buffer);
    if (!device.open(QIODevice::WriteOnly)) {
        result.diagnostic = "無法開啟編碼緩衝區";
        return result;
    }
    // JPEG 沒有 alpha。透明度另外走 /SMask，這裡先把來源攤成不透明的 RGB，
    // 否則 Qt 會依 alpha 預乘後再丟掉它，半透明處會多一層黑。
    QImage opaque = image.convertToFormat(QImage::Format_RGB888);
    if (!opaque.save(&device, "JPEG", quality)) {
        result.diagnostic = "JPEG 編碼失敗";
        return result;
    }
    device.close();

    result.data.assign(buffer.constData(), static_cast<std::size_t>(buffer.size()));
    result.filter = "DCTDecode";
    result.colorSpace = "DeviceRGB";
    result.width = pixels.width();
    result.height = pixels.height();
    result.bitsPerComponent = 8;
    if (hasTransparency(pixels)) result.softMaskData = encodeSoftMask(pixels);
    result.ok = !result.data.empty();
    if (!result.ok) result.diagnostic = "JPEG 編碼結果為空";
    return result;
}

EncodedImage encodeFlate(const PixelBuffer& pixels) {
    EncodedImage result;
    if (pixels.isNull()) {
        result.diagnostic = "來源影像為空";
        return result;
    }

    const bool gray = isGrayscale(pixels);
    const std::int32_t components = gray ? 1 : 3;
    std::string samples;
    samples.reserve(static_cast<std::size_t>(pixels.width()) *
                    static_cast<std::size_t>(pixels.height()) *
                    static_cast<std::size_t>(components));

    for (std::int32_t y = 0; y < pixels.height(); ++y) {
        const std::uint8_t* row = pixels.data() + pixels.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < pixels.width(); ++x) {
            if (gray) {
                samples.push_back(static_cast<char>(row[x * 4 + 0]));
            } else {
                samples.push_back(static_cast<char>(row[x * 4 + 2]));
                samples.push_back(static_cast<char>(row[x * 4 + 1]));
                samples.push_back(static_cast<char>(row[x * 4 + 0]));
            }
        }
    }

    result.data = zlibDeflate(samples);
    if (result.data.empty()) {
        result.diagnostic = "FlateDecode 壓縮失敗";
        return result;
    }
    result.filter = "FlateDecode";
    result.colorSpace = gray ? "DeviceGray" : "DeviceRGB";
    result.width = pixels.width();
    result.height = pixels.height();
    result.bitsPerComponent = 8;
    if (hasTransparency(pixels)) result.softMaskData = encodeSoftMask(pixels);
    result.ok = true;
    return result;
}

EncodedImage encodeImage(const PixelBuffer& pixels, const CompressionSettings& settings) {
    switch (settings.codec) {
        case ImageCodec::Jpeg:
            return encodeJpeg(pixels, settings.jpegQuality);
        case ImageCodec::Flate:
            return encodeFlate(pixels);
        case ImageCodec::Auto:
            break;
    }

    EncodedImage jpeg = encodeJpeg(pixels, settings.jpegQuality);
    EncodedImage flate = encodeFlate(pixels);
    if (!jpeg.ok) return flate;
    if (!flate.ok) return jpeg;
    return flate.byteSize() <= jpeg.byteSize() ? flate : jpeg;
}

PixelBuffer resample(const PixelBuffer& source, std::int32_t width, std::int32_t height) {
    if (source.isNull() || width <= 0 || height <= 0) return {};
    if (width == source.width() && height == source.height()) {
        PixelBuffer out(width, height);
        for (std::int32_t y = 0; y < height; ++y) {
            std::memcpy(out.scanline(y),
                        source.data() + source.stride() * static_cast<std::size_t>(y),
                        std::min(out.stride(), source.stride()));
        }
        return out;
    }

    const QImage scaled = wrapPixels(source).scaled(width, height, Qt::IgnoreAspectRatio,
                                                    Qt::SmoothTransformation);
    return fromQImage(scaled);
}

}  // namespace alioth::engine::enhance
