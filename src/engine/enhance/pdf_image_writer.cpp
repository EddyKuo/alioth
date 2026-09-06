#include "engine/enhance/pdf_image_writer.h"

#include <algorithm>
#include <utility>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::enhance {
namespace {

using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfStream;

[[nodiscard]] PdfDictionary baseImageDictionary(const EncodedImage& image) {
    PdfDictionary dict;
    dict.set("Type", objects::makeName("XObject"));
    dict.set("Subtype", objects::makeName("Image"));
    dict.set("Width", PdfObject{static_cast<std::int64_t>(image.width)});
    dict.set("Height", PdfObject{static_cast<std::int64_t>(image.height)});
    dict.set("ColorSpace", objects::makeName(image.colorSpace));
    dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(image.bitsPerComponent)});
    dict.set("Filter", objects::makeName(image.filter));
    return dict;
}

}  // namespace

PdfObject makeImageStream(const EncodedImage& image, int softMaskObjectNumber) {
    PdfStream stream;
    stream.dict = baseImageDictionary(image);
    if (softMaskObjectNumber > 0) {
        stream.dict.set("SMask", objects::makeRef(softMaskObjectNumber));
    }
    stream.data = image.data;
    return PdfObject{std::move(stream)};
}

ImageWriteResult writeImageObject(objects::IncrementalAppender& appender,
                                  const EncodedImage& image) {
    ImageWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟";
        return result;
    }
    if (!image.ok || image.data.empty() || image.width <= 0 || image.height <= 0) {
        result.diagnostic = image.diagnostic.empty() ? "影像編碼無效" : image.diagnostic;
        return result;
    }

    if (!image.softMaskData.empty()) {
        // /SMask 一律是 DeviceGray、8 位元、與主影像同尺寸。尺寸不同的話
        // 部分檢視器會自行縮放、部分直接忽略透明度，兩邊看到的畫面不一樣。
        PdfStream mask;
        mask.dict.set("Type", objects::makeName("XObject"));
        mask.dict.set("Subtype", objects::makeName("Image"));
        mask.dict.set("Width", PdfObject{static_cast<std::int64_t>(image.width)});
        mask.dict.set("Height", PdfObject{static_cast<std::int64_t>(image.height)});
        mask.dict.set("ColorSpace", objects::makeName("DeviceGray"));
        mask.dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});
        mask.dict.set("Filter", objects::makeName("FlateDecode"));
        mask.data = image.softMaskData;

        result.softMaskObject = appender.allocateObject();
        appender.setObject(result.softMaskObject, PdfObject{std::move(mask)});
    }

    result.imageObject = appender.allocateObject();
    appender.setObject(result.imageObject, makeImageStream(image, result.softMaskObject));
    result.ok = true;
    return result;
}

std::string drawImageContent(const std::string& resourceName, const domain::RectF& rect) {
    const double width = rect.width();
    const double height = rect.height();
    std::string content = "q\n";
    content += objects::formatReal(width) + " 0 0 " + objects::formatReal(height) + " " +
               objects::formatReal(rect.left) + " " + objects::formatReal(rect.bottom) + " cm\n";
    content += "/" + objects::escapeName(resourceName) + " Do\n";
    content += "Q\n";
    return content;
}

std::string fillRectContent(const domain::ColorRgb& color, const domain::RectF& rect) {
    std::string content = "q\n";
    content += objects::formatReal(color.r) + " " + objects::formatReal(color.g) + " " +
               objects::formatReal(color.b) + " rg\n";
    content += objects::formatReal(rect.left) + " " + objects::formatReal(rect.bottom) + " " +
               objects::formatReal(rect.width()) + " " + objects::formatReal(rect.height()) +
               " re\nf\n";
    content += "Q\n";
    return content;
}

bool pageBox(const objects::PdfSourceDocument& source, const objects::PdfRef& page,
             domain::RectF& out) {
    for (const char* key : {"CropBox", "MediaBox"}) {
        const PdfObject box = source.resolve(source.inheritedPageAttribute(page, key));
        const objects::PdfArray* values = box.asArray();
        if (values == nullptr || values->size() != 4) continue;
        const double x0 = source.resolve((*values)[0]).asNumber();
        const double y0 = source.resolve((*values)[1]).asNumber();
        const double x1 = source.resolve((*values)[2]).asNumber();
        const double y1 = source.resolve((*values)[3]).asNumber();
        // PDF 允許矩形以任一組對角給出，不保證左下在前。照抄順序會得到
        // 負寬高，之後的每一次縮放都會把圖翻面。
        out = domain::RectF{std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
        if (!out.isEmpty()) return true;
    }
    return false;
}

namespace {

[[nodiscard]] bool rewriteContents(objects::IncrementalAppender& appender,
                                   const objects::PdfRef& page, int contentObject, bool prepend) {
    PdfObject pageObject = appender.currentObject(page.number);
    PdfDictionary* dict = pageObject.asDictionary();
    if (dict == nullptr) return false;

    objects::PdfArray contents;
    if (prepend) {
        contents.push_back(objects::makeRef(contentObject));
        if (const PdfObject* existing = dict->find("Contents"); existing != nullptr) {
            if (existing->isRef()) {
                const PdfObject target = appender.currentObject(existing->asRef().number);
                if (const objects::PdfArray* array = target.asArray(); array != nullptr) {
                    for (const PdfObject& item : *array) contents.push_back(item);
                } else {
                    contents.push_back(*existing);
                }
            } else if (const objects::PdfArray* array = existing->asArray(); array != nullptr) {
                for (const PdfObject& item : *array) contents.push_back(item);
            } else if (!existing->isNull()) {
                // /Contents 必須是間接參照或其陣列。直接串流是損毀的檔案，
                // 這時放棄比猜測安全：猜錯會把原有內容整段丟掉。
                return false;
            }
        }
    } else {
        contents.push_back(objects::makeRef(contentObject));
    }

    dict->set("Contents", PdfObject{std::move(contents)});
    return appender.updateObject(page.number, std::move(pageObject));
}

}  // namespace

bool prependPageContent(objects::IncrementalAppender& appender, const objects::PdfRef& page,
                        int contentObject) {
    return rewriteContents(appender, page, contentObject, true);
}

bool replacePageContent(objects::IncrementalAppender& appender, const objects::PdfRef& page,
                        int contentObject) {
    return rewriteContents(appender, page, contentObject, false);
}

}  // namespace alioth::engine::enhance
