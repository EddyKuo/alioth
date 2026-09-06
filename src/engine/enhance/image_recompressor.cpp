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

// 把 /SMask 併回 alpha 通道。這一步是「遮罩要一起處理」的實作核心：
// 併回來之後，重編碼的輸出會自己帶出一份尺寸必然正確的新 /SMask。
void applySoftMask(PixelBuffer& pixels, const PixelBuffer& mask) {
    if (mask.isNull() || pixels.isNull()) return;
    const PixelBuffer sized = (mask.width() == pixels.width() && mask.height() == pixels.height())
                                  ? clonePixels(mask)
                                  : resample(mask, pixels.width(), pixels.height());
    if (sized.isNull()) return;
    for (std::int32_t y = 0; y < pixels.height(); ++y) {
        std::uint8_t* row = pixels.scanline(y);
        const std::uint8_t* maskRow = sized.data() + sized.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < pixels.width(); ++x) {
            row[x * 4 + 3] = maskRow[x * 4 + 0];
        }
    }
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
            if (info.colourKeyMask) {
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

            // /SMask 的位元組也算進「原本佔多少」，否則替換之後總量看起來變大，
            // 而使用者看到的是檔案變小——兩個數字對不起來就沒有人會相信報告。
            int softMaskObject = 0;
            if (settings.includeMasks) {
                if (const PdfObject* smask = stream->dict.find("SMask");
                    smask != nullptr && smask->isRef()) {
                    softMaskObject = smask->asRef().number;
                    const PdfObject maskObject = source.object(softMaskObject);
                    if (const PdfStream* maskStream = maskObject.asStream(); maskStream != nullptr) {
                        report.originalBytes += static_cast<std::int64_t>(maskStream->data.size());
                        ImageInfo maskInfo;
                        if (readImageInfo(source, maskStream->dict, maskInfo)) {
                            const DecodedImage mask = decodeImageObject(source, *maskStream, maskInfo);
                            if (mask.ok) applySoftMask(decoded.pixels, mask.pixels);
                        }
                    }
                }
            }

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
            int newMaskObject = 0;
            if (!encoded.softMaskData.empty()) {
                PdfStream mask;
                mask.dict.set("Type", objects::makeName("XObject"));
                mask.dict.set("Subtype", objects::makeName("Image"));
                mask.dict.set("Width", PdfObject{static_cast<std::int64_t>(encoded.width)});
                mask.dict.set("Height", PdfObject{static_cast<std::int64_t>(encoded.height)});
                mask.dict.set("ColorSpace", objects::makeName("DeviceGray"));
                mask.dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});
                mask.dict.set("Filter", objects::makeName("FlateDecode"));
                mask.data = encoded.softMaskData;

                if (softMaskObject > 0) {
                    newMaskObject = softMaskObject;
                    if (!appender.updateObject(softMaskObject, PdfObject{std::move(mask)})) {
                        report.decision = RecompressDecision::Failed;
                        result.images.push_back(report);
                        continue;
                    }
                } else {
                    newMaskObject = appender.allocateObject();
                    appender.setObject(newMaskObject, PdfObject{std::move(mask)});
                }
            }

            if (!appender.updateObject(objectNumber, makeImageStream(encoded, newMaskObject))) {
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
