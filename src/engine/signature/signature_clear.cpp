#include "engine/signature/signature_clear.h"

#include <set>
#include <vector>

#include "engine/objects/incremental_appender.h"
#include "engine/objects/page_object_editor.h"

namespace alioth::engine::signature {
namespace {

using objects::IncrementalAppender;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;

ClearSignaturesResult failure(std::string message) {
    ClearSignaturesResult result;
    result.diagnostic = std::move(message);
    return result;
}

// 解析可能是間接參照的值。
PdfObject resolve(const IncrementalAppender& appender, const PdfObject& object) {
    return object.isRef() ? appender.currentObject(object.asRef().number) : object;
}

// 這個欄位是不是簽章欄位。
//
// /FT 可以繼承自父欄位，所以找不到時要往 /Parent 走。只看自己那一層的話，
// 「父欄位帶 /FT /Sig、子 widget 不帶」這種很常見的寫法會被漏掉，
// 而漏掉的後果是清完之後文件裡還留著一個簽章欄位。
bool isSignatureField(const IncrementalAppender& appender, const PdfDictionary& field,
                      int depth = 0) {
    if (depth > 8) return false;  // 壞檔可能有環，別跟著它轉
    if (const PdfObject* type = field.find("FT"); type != nullptr) {
        return type->isName("Sig");
    }
    const PdfObject* parent = field.find("Parent");
    if (parent == nullptr || !parent->isRef()) return false;
    const PdfObject resolved = appender.currentObject(parent->asRef().number);
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return false;
    return isSignatureField(appender, *dict, depth + 1);
}

// 一個簽章欄位牽涉到的所有物件編號：欄位本身，加上它的 /Kids（widget 分開寫時）。
void collectFieldObjects(const IncrementalAppender& appender, int fieldNumber,
                         std::set<int>& out, int depth = 0) {
    if (depth > 8) return;
    if (!out.insert(fieldNumber).second) return;

    const PdfObject object = appender.currentObject(fieldNumber);
    const PdfDictionary* dict = object.asDictionary();
    if (dict == nullptr) return;
    const PdfObject* kids = dict->find("Kids");
    if (kids == nullptr) return;
    const PdfObject resolved = resolve(appender, *kids);
    const PdfArray* array = resolved.asArray();
    if (array == nullptr) return;
    for (const PdfObject& kid : *array) {
        if (kid.isRef()) collectFieldObjects(appender, kid.asRef().number, out, depth + 1);
    }
}

}  // namespace

ClearSignaturesResult clearSignatureFields(const std::string& sourceBytes) {
    IncrementalAppender appender;
    std::string diagnostic;
    if (appender.open(sourceBytes, &diagnostic) != objects::SourceStatus::Ok) {
        return failure(diagnostic.empty() ? "無法解析文件" : diagnostic);
    }

    const PdfObject* root = appender.source().trailer().find("Root");
    if (root == nullptr || !root->isRef()) return failure("原檔的 trailer 沒有可用的 /Root");
    const PdfRef catalogRef = root->asRef();

    const PdfObject catalog = appender.currentObject(catalogRef.number);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) return failure("/Root 指向的不是字典");

    const PdfObject* acroFormValue = catalogDict->find("AcroForm");
    if (acroFormValue == nullptr) {
        // 沒有表單就沒有簽章欄位。這不是錯誤——呼叫端據 changedAnything()
        // 判斷要不要寫檔。
        ClearSignaturesResult result;
        result.ok = true;
        return result;
    }

    const int acroFormObject = acroFormValue->isRef() ? acroFormValue->asRef().number : 0;
    const PdfObject resolvedForm = resolve(appender, *acroFormValue);
    const PdfDictionary* formDict = resolvedForm.asDictionary();
    if (formDict == nullptr) return failure("/AcroForm 不是字典");

    const PdfObject* fieldsValue = formDict->find("Fields");
    if (fieldsValue == nullptr) {
        ClearSignaturesResult result;
        result.ok = true;
        return result;
    }
    const int fieldsObject = fieldsValue->isRef() ? fieldsValue->asRef().number : 0;
    const PdfObject resolvedFields = resolve(appender, *fieldsValue);
    const PdfArray* fields = resolvedFields.asArray();
    if (fields == nullptr) return failure("/AcroForm /Fields 不是陣列");

    ClearSignaturesResult result;

    // 一、挑出要移除的欄位，順便收齊它們底下的 widget 物件編號。
    std::set<int> doomed;
    PdfArray keptFields;
    for (const PdfObject& entry : *fields) {
        if (!entry.isRef()) {
            keptFields.push_back(entry);
            continue;
        }
        const int number = entry.asRef().number;
        const PdfObject object = appender.currentObject(number);
        const PdfDictionary* dict = object.asDictionary();
        if (dict == nullptr || !isSignatureField(appender, *dict)) {
            keptFields.push_back(entry);
            continue;
        }
        collectFieldObjects(appender, number, doomed);
        ++result.removedFields;
    }

    if (result.removedFields == 0) {
        result.ok = true;
        return result;
    }

    // 二、從各頁的 /Annots 摘掉對應的 widget。
    const auto& pages = appender.source().pages();
    for (const PdfRef& page : pages) {
        for (const int annotation : objects::pageAnnotationRefs(appender.source(), page)) {
            if (doomed.count(annotation) == 0) continue;
            const objects::PageEditStatus status =
                objects::removeFromPageArray(appender, page, "Annots", annotation);
            if (!status.ok) return failure("移除 widget 失敗：" + status.diagnostic);
            ++result.removedWidgets;
        }
    }

    // 三、改寫 /Fields，並在沒有欄位剩下時把 /SigFlags 一起拿掉。
    //
    // 留著 /SigFlags 的話，Acrobat 會認為這份文件仍然「含簽章欄位」並據此
    // 限制某些操作——一個指向不存在欄位的旗標，症狀是使用者被擋下來卻
    // 找不到是哪個欄位造成的。
    PdfDictionary updatedForm = *formDict;
    if (fieldsObject != 0) {
        if (!appender.updateObject(fieldsObject, PdfObject{keptFields})) {
            return failure("/Fields 陣列物件不存在於原檔");
        }
    } else {
        updatedForm.set("Fields", PdfObject{keptFields});
    }
    if (keptFields.empty()) updatedForm.remove("SigFlags");

    if (acroFormObject != 0) {
        if (!appender.updateObject(acroFormObject, PdfObject{updatedForm})) {
            return failure("/AcroForm 物件不存在於原檔");
        }
    } else {
        PdfDictionary updatedCatalog = *catalogDict;
        updatedCatalog.set("AcroForm", PdfObject{updatedForm});
        if (!appender.updateObject(catalogRef.number, PdfObject{updatedCatalog})) {
            return failure("catalog 物件不存在於原檔");
        }
    }

    const objects::BuildResult built = appender.build();
    if (!built.ok) return failure("增量儲存失敗：" + built.diagnostic);

    result.ok = true;
    result.bytes = built.bytes;
    return result;
}

}  // namespace alioth::engine::signature
