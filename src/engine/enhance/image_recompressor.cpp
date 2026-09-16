#include "engine/enhance/image_recompressor.h"

#include <algorithm>
#include <set>
#include <utility>

#include "engine/enhance/image_codec.h"
#include "engine/enhance/image_ops.h"
#include "engine/enhance/pdf_image_writer.h"
#include "engine/objects/pdf_parser.h"

namespace alioth::engine::enhance {
namespace {

using domain::enhance::ImageRecompressReport;
using domain::enhance::RecompressDecision;
using domain::enhance::RecompressSettings;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::PdfStream;

struct ImageInfo {
    std::string filter;
    std::string colorSpace;
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t bitsPerComponent{8};
    bool colourKeyMask{false};
};

[[nodiscard]] std::string singleFilterName(const PdfSourceDocument& source, const PdfObject& value) {
    const PdfObject resolved = source.resolve(value);
    if (resolved.isName()) return resolved.asName();
    if (const PdfArray* array = resolved.asArray(); array != nullptr && array->size() == 1) {
        return source.resolve((*array)[0]).asName();
    }
    return {};  // 空代表沒有濾鏡或串接了多個濾鏡（後者一律不處理）
}

[[nodiscard]] bool readImageInfo(const PdfSourceDocument& source, const PdfDictionary& dict,
                                 ImageInfo& out) {
    const PdfObject* subtype = dict.find("Subtype");
    if (subtype == nullptr || !source.resolve(*subtype).isName("Image")) return false;

    const PdfObject* width = dict.find("Width");
    const PdfObject* height = dict.find("Height");
    if (width == nullptr || height == nullptr) return false;
    out.width = static_cast<std::int32_t>(source.resolve(*width).asInteger());
    out.height = static_cast<std::int32_t>(source.resolve(*height).asInteger());

    if (const PdfObject* bits = dict.find("BitsPerComponent"); bits != nullptr) {
        out.bitsPerComponent = static_cast<std::int32_t>(source.resolve(*bits).asInteger());
    }
    if (const PdfObject* filter = dict.find("Filter"); filter != nullptr) {
        out.filter = singleFilterName(source, *filter);
    }
    if (const PdfObject* space = dict.find("ColorSpace"); space != nullptr) {
        out.colorSpace = source.resolve(*space).asName();
    }
    if (const PdfObject* mask = dict.find("Mask"); mask != nullptr) {
        // 色鍵遮罩（/Mask 是數字陣列）記的是原始取樣值。重新編碼之後那些值
        // 全都變了，遮罩會開始挖掉不該挖的地方，而且畫面上看起來像是
        // 影像本身破損。這種影像一律不動。
        out.colourKeyMask = source.resolve(*mask).asArray() != nullptr;
    }
    return out.width > 0 && out.height > 0;
}

[[nodiscard]] DecodedImage decodeImageObject(const PdfSourceDocument& source, const PdfStream& stream,
                                             const ImageInfo& info) {
    if (info.filter == "DCTDecode") return decodeJpeg(stream.data);
    if (info.filter.empty() || info.filter == "FlateDecode") {
        const objects::DecodeResult decoded = objects::decodeStream(
            stream, [&source](const PdfRef& ref) { return source.object(ref.number); });
        if (!decoded.ok) {
            DecodedImage failed;
            failed.diagnostic = decoded.diagnostic;
            return failed;
        }
        return decodeRawSamples(decoded.data, info.width, info.height, info.bitsPerComponent,
                                info.colorSpace);
    }
    DecodedImage unsupported;
    unsupported.diagnostic = "不支援的濾鏡：" + info.filter;
    return unsupported;
}

}  // namespace

RecompressResult recompressImages(objects::IncrementalAppender& appender,
                                  const std::vector<std::int32_t>& pages,
                                  const RecompressSettings& settings) {
    RecompressResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟";
        return result;
    }
    if (!settings.compression.valid()) {
        result.diagnostic = "壓縮設定不合法";
        return result;
    }

    const PdfSourceDocument& source = appender.source();
    const auto pageCount = static_cast<std::int32_t>(source.pages().size());
    std::set<int> visited;

    for (std::int32_t index = 0; index < pageCount; ++index) {
        if (!pages.empty() && std::find(pages.begin(), pages.end(), index) == pages.end()) continue;

        const PdfRef page = source.pages()[static_cast<std::size_t>(index)];
        const PdfObject resources =
            source.resolve(source.inheritedPageAttribute(page, "Resources"));
        const PdfDictionary* resourceDict = resources.asDictionary();
        if (resourceDict == nullptr) continue;

        const PdfObject* xobjectEntry = resourceDict->find("XObject");
        if (xobjectEntry == nullptr) continue;
        const PdfObject xobjects = source.resolve(*xobjectEntry);
        const PdfDictionary* xobjectDict = xobjects.asDictionary();
        if (xobjectDict == nullptr) continue;

        for (const auto& [name, value] : xobjectDict->entries()) {
            if (!value.isRef()) continue;  // 直接寫在資源裡的串流不合法
            const int objectNumber = value.asRef().number;
            if (!visited.insert(objectNumber).second) continue;

            const PdfObject imageObject = source.object(objectNumber);
            const PdfStream* stream = imageObject.asStream();
            if (stream == nullptr) continue;

            ImageInfo info;
            if (!readImageInfo(source, stream->dict, info)) continue;

            ImageRecompressReport report;
            report.resourceName = name;
            report.objectNumber = objectNumber;
            report.width = info.width;
            report.height = info.height;
            report.originalBytes = static_cast<std::int64_t>(stream->data.size());

            const auto pixelCount =
                static_cast<std::int64_t>(info.width) * static_cast<std::int64_t>(info.height);
            if (pixelCount < settings.minPixelCount) {
                report.decision = RecompressDecision::SkippedTooSmall;
                result.images.push_back(report);
                continue;
            }
            // These sample interpretations are not implemented by decodeRawSamples.
            // Replacing their dictionary would silently change the rendered image.
            if (info.colourKeyMask || stream->dict.find("Decode") != nullptr ||
                stream->dict.find("ImageMask") != nullptr ||
                stream->dict.find("SMaskInData") != nullptr) {
                report.decision = RecompressDecision::SkippedUnsupported;
                result.images.push_back(report);
                continue;
            }

            DecodedImage decoded = decodeImageObject(source, *stream, info);
            if (!decoded.ok) {
                report.decision = info.filter == "DCTDecode" || info.filter.empty() ||
                                          info.filter == "FlateDecode"
                                      ? RecompressDecision::Failed
                                      : RecompressDecision::SkippedUnsupported;
                result.images.push_back(report);
                continue;
            }

            // Dimensions do not change in this operation. Keep masks verbatim:
            // decoding failure must never make an image opaque, and masks can
            // be shared with images on pages outside the requested range.

            PixelBuffer pixels = std::move(decoded.pixels);
            const EncodedImage encoded = encodeImage(pixels, settings.compression);
            if (!encoded.ok) {
                report.decision = RecompressDecision::Failed;
                result.images.push_back(report);
                continue;
            }

            report.newBytes = encoded.byteSize();
            report.decision =
                domain::enhance::decideReplacement(report.originalBytes, report.newBytes, settings);
            if (report.decision != RecompressDecision::Replaced) {
                result.images.push_back(report);
                continue;
            }

            // 就地覆寫影像物件：資源名稱與所有引用都不需要改，
            // 被多頁共用的影像也因此一次到位。
            PdfObject replacement = makeImageStream(encoded);
            // Preserve rendering metadata as well as both forms of masks.
            for (const char* key : {"SMask", "Mask", "Interpolate", "Intent", "OC",
                                    "StructParent", "Metadata"}) {
                if (const PdfObject* entry = stream->dict.find(key)) {
                    replacement.asStream()->dict.set(key, *entry);
                }
            }
            if (!appender.updateObject(objectNumber, std::move(replacement))) {
                report.decision = RecompressDecision::Failed;
                result.images.push_back(report);
                continue;
            }
            result.images.push_back(report);
        }
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::enhance
