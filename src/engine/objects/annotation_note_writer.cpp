#include "engine/objects/annotation_note_writer.h"

#include <utility>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::objects {
namespace {

// 外觀串流裡畫的就是 /Contents 的子型。改了文字卻不重畫 /AP，就會出現
// 「Acrobat 顯示舊字、其他檢視器顯示新字」的分岔。
[[nodiscard]] bool appearanceDependsOnContents(const std::string& subtype) {
    return subtype == "FreeText" || subtype == "Redact";
}

}  // namespace

NoteEditResult setAnnotationContents(IncrementalAppender& appender, int annotationObjectNumber,
                                     const std::string& contents,
                                     const std::string& modifiedDate) {
    if (!appender.isOpen()) return {false, "附加器尚未開啟原檔"};

    PdfObject current = appender.currentObject(annotationObjectNumber);
    // 串流物件也有字典，所以要先擋掉——外觀串流（/Subtype /Form）的字典長得
    // 夠像註解，光看 /Subtype 會讓「把註釋寫進 /AP」這種錯誤靜靜通過。
    // 註解本身在 PDF 裡從來不是串流。
    if (current.isStream()) {
        return {false, "指定編號是串流物件，不是註解字典"};
    }
    PdfDictionary* dict = current.asDictionary();
    if (dict == nullptr) {
        return {false, "找不到指定編號的註解字典：" + std::to_string(annotationObjectNumber)};
    }
    if (const PdfObject* type = dict->find("Type"); type != nullptr && !type->isName("Annot")) {
        return {false, "/Type 不是 /Annot，拒絕當成註解改寫"};
    }

    std::string subtype;
    if (const PdfObject* value = dict->find("Subtype"); value != nullptr && value->isName()) {
        subtype = value->asName();
    }
    if (subtype.empty()) return {false, "這個物件沒有 /Subtype，不像是註解"};
    if (subtype == "Popup") {
        // /Popup 是父註解的視窗，它自己沒有註釋文字。寫進去不會有任何檢視器讀，
        // 但會讓註解數量與內容對不上。
        return {false, "/Popup 沒有註釋文字，請改寫它的父註解"};
    }
    if (appearanceDependsOnContents(subtype)) {
        return {false, "/" + subtype + " 的外觀串流由文字決定，改字必須一併重畫 /AP"};
    }

    if (contents.empty()) {
        dict->remove("Contents");
    } else {
        dict->set("Contents", makeTextString(contents));
    }
    if (!modifiedDate.empty()) dict->set("M", makeLiteralString(modifiedDate));

    // updateObject() 只認原檔裡就有的物件。改寫「這一輪剛寫進去、還沒落盤」的
    // 註解時它會失敗，而那是合法的情境——同一次操作裡建立便利貼再填內容。
    // 那種編號一定來自 allocateObject()，所以退回 setObject() 是對的：
    // 它蓋掉的是同一個待寫入項目，不會多出一個物件。
    if (!appender.updateObject(annotationObjectNumber, current)) {
        appender.setObject(annotationObjectNumber, std::move(current));
    }
    return {true, {}};
}

}  // namespace alioth::engine::objects
