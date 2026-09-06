#include "engine/objects/struct_alt_text_writer.h"

namespace alioth::engine::objects {

AltTextEditStatus setAlternateText(IncrementalAppender& appender, int structElementObjectNumber,
                                   const std::string& altTextUtf8) {
    if (structElementObjectNumber <= 0) {
        return AltTextEditStatus{
            false, "這個結構元素是直接物件（沒有自己的物件編號），無法單獨改寫；"
                   "需要重寫擁有它的父物件，超出這個通道的支援範圍"};
    }

    const PdfObject current = appender.currentObject(structElementObjectNumber);
    const PdfDictionary* dict = current.asDictionary();
    if (dict == nullptr) {
        return AltTextEditStatus{false, "指定的物件編號不是字典，不是結構元素"};
    }
    if (!dict->has("S")) {
        return AltTextEditStatus{false, "指定的物件沒有 /S，不是結構元素"};
    }

    PdfDictionary updated = *dict;
    if (altTextUtf8.empty()) {
        updated.remove("Alt");
    } else {
        updated.set("Alt", makeTextString(altTextUtf8));
    }

    if (!appender.updateObject(structElementObjectNumber, PdfObject{std::move(updated)})) {
        return AltTextEditStatus{false, "結構元素物件不存在於原檔"};
    }
    return AltTextEditStatus{true, {}};
}

}  // namespace alioth::engine::objects
