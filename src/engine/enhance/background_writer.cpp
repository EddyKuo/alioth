#include "engine/enhance/background_writer.h"

#include <cmath>
#include <numbers>
#include <utility>

#include "engine/enhance/image_codec.h"
#include "engine/enhance/pdf_image_writer.h"
#include "engine/objects/page_object_editor.h"
#include "engine/objects/pdf_object.h"

namespace alioth::engine::enhance {
namespace {

using domain::enhance::BackgroundSource;
using domain::enhance::BackgroundSpec;
using domain::enhance::CompressionSettings;
using objects::PdfDictionary;
using objects::PdfObject;

// 資源名稱刻意帶產品前綴。原檔已經有 /Im0 /GS0 是常態，撞名的結果是
// 頁面原本的影像被換成我們的背景——那是資料損毀，不是顯示問題。
constexpr const char* kImageResource = "AliothBgIm";
constexpr const char* kStateResource = "AliothBgGS";

[[nodiscard]] PdfObject makeAlphaState(double opacity) {
    PdfDictionary state;
    state.set("Type", objects::makeName("ExtGState"));
    // 填色與描邊的 alpha 都要設。只設 /ca 的話，背景若含描邊指令
    // （影像不會，但純色矩形的實作日後若改用描邊就會）透明度會不一致。
    state.set("ca", PdfObject{opacity});
    state.set("CA", PdfObject{opacity});
    return PdfObject{std::move(state)};
}

// 繞矩形中心旋轉後再畫。旋轉的是背景本身，不是頁面，
// 因此矩形位置由 backgroundPlacement 決定之後才套旋轉。
[[nodiscard]] std::string rotatedImageContent(const std::string& resource, const domain::RectF& rect,
                                              double rotationDeg) {
    if (std::abs(rotationDeg) < 1e-9) return drawImageContent(resource, rect);

    const double radians = rotationDeg * std::numbers::pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double cx = (rect.left + rect.right) / 2.0;
    const double cy = (rect.bottom + rect.top) / 2.0;
    const double w = rect.width();
    const double h = rect.height();

    std::string content = "q\n";
    content += "1 0 0 1 " + objects::formatReal(cx) + " " + objects::formatReal(cy) + " cm\n";
    content += objects::formatReal(c) + " " + objects::formatReal(s) + " " +
               objects::formatReal(-s) + " " + objects::formatReal(c) + " 0 0 cm\n";
    content += objects::formatReal(w) + " 0 0 " + objects::formatReal(h) + " " +
               objects::formatReal(-w / 2.0) + " " + objects::formatReal(-h / 2.0) + " cm\n";
    content += "/" + objects::escapeName(resource) + " Do\n";
    content += "Q\n";
    return content;
}

}  // namespace

BackgroundResult addBackground(objects::IncrementalAppender& appender, const BackgroundSpec& spec,
                               const CompressionSettings& compression) {
    BackgroundResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟";
        return result;
    }
    if (!spec.valid()) {
        result.diagnostic = "背景設定不合法";
        return result;
    }

    domain::SizeF imageSize{};
    if (spec.source == BackgroundSource::Image) {
        const std::string bytes(reinterpret_cast<const char*>(spec.imageBytes.data()),
                                spec.imageBytes.size());
        const DecodedImage decoded = decodeImageFile(bytes);
        if (!decoded.ok) {
            result.diagnostic = decoded.diagnostic;
            return result;
        }
        const EncodedImage encoded = encodeImage(decoded.pixels, compression);
        if (!encoded.ok) {
            result.diagnostic = encoded.diagnostic;
            return result;
        }
        const ImageWriteResult written = writeImageObject(appender, encoded);
        if (!written.ok) {
            result.diagnostic = written.diagnostic;
            return result;
        }
        result.imageObject = written.imageObject;
        imageSize = domain::SizeF{static_cast<double>(encoded.width),
                                  static_cast<double>(encoded.height)};
    }

    const bool needsAlpha = spec.opacity < 1.0;
    const auto pageCount = static_cast<std::int32_t>(appender.source().pages().size());

    for (std::int32_t index = 0; index < pageCount; ++index) {
        if (!spec.appliesToPage(index)) continue;

        objects::PdfRef page{};
        if (!objects::pageRefAt(appender, index, page)) continue;

        domain::RectF box{};
        if (!pageBox(appender.source(), page, box)) {
            result.diagnostic = "頁面缺少可用的 /MediaBox";
            return result;
        }

        const domain::RectF placement =
            domain::enhance::backgroundPlacement(box.size(), imageSize, spec);
        if (placement.isEmpty()) continue;

        // backgroundPlacement 以頁面尺寸為基準算出相對位置，這裡再平移到
        // 實際的框原點。/MediaBox 不從 (0,0) 起算的檔案並不罕見，
        // 忽略原點的話背景會整片偏移，而在 A4 上偏移量剛好小到像是留白設定錯誤。
        const domain::RectF rect{placement.left + box.left, placement.bottom + box.bottom,
                                 placement.right + box.left, placement.top + box.bottom};

        std::string content = "q\n";
        if (needsAlpha) content += "/" + objects::escapeName(kStateResource) + " gs\n";
        if (spec.source == BackgroundSource::SolidColor) {
            content += fillRectContent(spec.color, rect);
        } else {
            content += rotatedImageContent(kImageResource, rect, spec.rotationDeg);
        }
        content += "Q\n";

        objects::PdfStream stream;
        stream.data = std::move(content);
        const int contentObject = appender.allocateObject();
        appender.setObject(contentObject, PdfObject{std::move(stream)});

        if (spec.source == BackgroundSource::Image) {
            const objects::PageEditStatus status = objects::setPageResource(
                appender, page, "XObject", kImageResource, objects::makeRef(result.imageObject));
            if (!status.ok) {
                result.diagnostic = status.diagnostic;
                return result;
            }
        }
        if (needsAlpha) {
            const objects::PageEditStatus status = objects::setPageResource(
                appender, page, "ExtGState", kStateResource, makeAlphaState(spec.opacity));
            if (!status.ok) {
                result.diagnostic = status.diagnostic;
                return result;
            }
        }

        if (!prependPageContent(appender, page, contentObject)) {
            result.diagnostic = "無法將背景插入頁面內容之前";
            return result;
        }
        result.pagesChanged.push_back(index);
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::enhance
