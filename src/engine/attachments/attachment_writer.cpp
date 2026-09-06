#include "engine/attachments/attachment_writer.h"

#include <map>
#include <variant>

#include "engine/attachments/attachment_reader.h"
#include "engine/objects/pdf_object.h"

namespace alioth::engine::attachments {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfName;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfStream;
using objects::PdfString;

int catalogNumber(const objects::PdfSourceDocument& source) {
    const PdfObject* root = source.trailer().find("Root");
    if (root == nullptr || !root->isRef()) return 0;
    return root->asRef().number;
}

// 既有的文件層附件：合併寫回時要保留它們，否則加一個附件等於刪掉其餘全部。
// 只取名稱與 filespec 的參照，不重寫內容——內容的位元組留在原處就好。
std::map<std::string, PdfObject> existingEmbeddedFiles(const objects::PdfSourceDocument& source) {
    std::map<std::string, PdfObject> result;
    const int catalog = catalogNumber(source);
    if (catalog <= 0) return result;
    const PdfObject catalogObject = source.object(catalog);
    const PdfDictionary* catalogDict = catalogObject.asDictionary();
    if (catalogDict == nullptr) return result;
    const PdfObject* names = catalogDict->find("Names");
    if (names == nullptr) return result;
    const PdfObject resolvedNames = source.resolve(*names);
    const PdfDictionary* namesDict = resolvedNames.asDictionary();
    if (namesDict == nullptr) return result;
    const PdfObject* embedded = namesDict->find("EmbeddedFiles");
    if (embedded == nullptr) return result;

    // 只走一層 /Names；有 /Kids 的深樹交給下面的走訪。
    struct Frame {
        PdfObject node;
        int depth;
    };
    std::vector<Frame> stack{{*embedded, 0}};
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.depth >= 32) continue;
        const PdfObject resolved = source.resolve(frame.node);
        const PdfDictionary* dict = resolved.asDictionary();
        if (dict == nullptr) continue;
        if (const PdfObject* entries = dict->find("Names")) {
            const PdfObject resolvedEntries = source.resolve(*entries);
            if (const PdfArray* array = resolvedEntries.asArray()) {
                for (std::size_t i = 0; i + 1 < array->size(); i += 2) {
                    const PdfObject key = source.resolve((*array)[i]);
                    if (const PdfString* text = std::get_if<PdfString>(&key.value())) {
                        result.emplace(text->bytes, (*array)[i + 1]);
                    }
                }
            }
        }
        if (const PdfObject* kids = dict->find("Kids")) {
            const PdfObject resolvedKids = source.resolve(*kids);
            if (const PdfArray* array = resolvedKids.asArray()) {
                for (const PdfObject& kid : *array) stack.push_back({kid, frame.depth + 1});
            }
        }
    }
    return result;
}

}  // namespace

AttachmentWriteResult addAttachment(objects::IncrementalAppender& appender,
                                    const AttachmentSpec& spec) {
    AttachmentWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開啟";
        return result;
    }
    if (spec.fileName.empty()) {
        // 沒有名字的附件在面板上只會顯示「（未命名）」，而使用者無從分辨兩個未命名的。
        result.diagnostic = "附件必須有檔名";
        return result;
    }
    if (spec.bytes.empty()) {
        result.diagnostic = "附件內容是空的";
        return result;
    }
    if (spec.bytes.size() > kMaxAttachmentBytes) {
        result.diagnostic = "附件超過大小上限";
        return result;
    }

    const int catalog = catalogNumber(appender.source());
    if (catalog <= 0) {
        result.diagnostic = "找不到 catalog";
        return result;
    }

    // 1. 內嵌檔案串流。
    PdfDictionary params;
    params.set("Size", PdfObject{static_cast<std::int64_t>(spec.bytes.size())});

    PdfStream stream;
    stream.dict.set("Type", PdfObject{PdfName{"EmbeddedFile"}});
    if (!spec.mimeType.empty()) {
        // /Subtype 是名稱物件，斜線要以 #2F 表示——序列化器負責跳脫，這裡給原文。
        stream.dict.set("Subtype", PdfObject{PdfName{spec.mimeType}});
    }
    stream.dict.set("Params", PdfObject{std::move(params)});
    stream.data = spec.bytes;

    result.streamObject = appender.allocateObject();
    appender.setObject(result.streamObject, PdfObject{std::move(stream)});

    // 2. filespec。/F 與 /UF 都寫：只寫其中一種，另一種解析器會顯示空檔名。
    PdfDictionary embeddedFiles;
    embeddedFiles.set("F", PdfObject{PdfRef{result.streamObject, 0}});
    embeddedFiles.set("UF", PdfObject{PdfRef{result.streamObject, 0}});

    PdfDictionary fileSpec;
    fileSpec.set("Type", PdfObject{PdfName{"Filespec"}});
    fileSpec.set("F", PdfObject{PdfString{spec.fileName, false}});
    fileSpec.set("UF", PdfObject{PdfString{spec.fileName, false}});
    if (!spec.description.empty()) {
        fileSpec.set("Desc", PdfObject{PdfString{spec.description, false}});
    }
    fileSpec.set("EF", PdfObject{std::move(embeddedFiles)});

    result.fileSpecObject = appender.allocateObject();
    appender.setObject(result.fileSpecObject, PdfObject{std::move(fileSpec)});

    if (spec.pageIndex >= 0) {
        // 3a. 檔案附件註解。
        const std::vector<PdfRef>& pages = appender.source().pages();
        if (spec.pageIndex >= static_cast<std::int32_t>(pages.size())) {
            result.diagnostic = "頁碼超出範圍";
            return result;
        }
        const PdfRef pageRef = pages[static_cast<std::size_t>(spec.pageIndex)];

        PdfArray rect;
        rect.push_back(PdfObject{spec.rectPt.left});
        rect.push_back(PdfObject{spec.rectPt.bottom});
        rect.push_back(PdfObject{spec.rectPt.right});
        rect.push_back(PdfObject{spec.rectPt.top});

        PdfDictionary annot;
        annot.set("Type", PdfObject{PdfName{"Annot"}});
        annot.set("Subtype", PdfObject{PdfName{"FileAttachment"}});
        annot.set("Rect", PdfObject{std::move(rect)});
        annot.set("FS", PdfObject{PdfRef{result.fileSpecObject, 0}});
        // /Name 決定圖示。PushPin 是四種標準圖示裡最不容易被誤認成別種註解的。
        annot.set("Name", PdfObject{PdfName{"PushPin"}});
        if (!spec.author.empty()) annot.set("T", PdfObject{PdfString{spec.author, false}});
        if (!spec.description.empty()) {
            annot.set("Contents", PdfObject{PdfString{spec.description, false}});
        }
        // /F 4 = Print：附件圖示預設要列印得出來，否則紙本審閱者不會知道有附件。
        annot.set("F", PdfObject{static_cast<std::int64_t>(4)});

        result.annotationObject = appender.allocateObject();
        appender.setObject(result.annotationObject, PdfObject{std::move(annot)});

        // 掛上頁面的 /Annots。用 currentObject 而不是 source().object：
        // 同一次附加裡若已經加過別的註解，直接讀原檔會把它抹掉。
        PdfObject page = appender.currentObject(pageRef.number);
        PdfDictionary* pageDict = page.asDictionary();
        if (pageDict == nullptr) {
            result.diagnostic = "頁面物件不是字典";
            return result;
        }
        PdfArray annots;
        if (const PdfObject* existing = pageDict->find("Annots")) {
            const PdfObject resolved = appender.source().resolve(*existing);
            if (const PdfArray* array = resolved.asArray()) annots = *array;
        }
        annots.push_back(PdfObject{PdfRef{result.annotationObject, 0}});
        pageDict->set("Annots", PdfObject{std::move(annots)});
        if (!appender.updateObject(pageRef.number, std::move(page))) {
            result.diagnostic = "無法更新頁面的 /Annots";
            return result;
        }
        result.ok = true;
        return result;
    }

    // 3b. 文件層附件：寫回 /Names /EmbeddedFiles。
    std::map<std::string, PdfObject> entries = existingEmbeddedFiles(appender.source());
    // 同名時後者取代前者。名稱樹的鍵必須唯一，留兩個相同的鍵會讓
    // 用二分搜尋的解析器找不到後面的項目。
    entries[spec.fileName] = PdfObject{PdfRef{result.fileSpecObject, 0}};

    PdfArray names;
    for (const auto& [name, value] : entries) {
        names.push_back(PdfObject{PdfString{name, false}});
        names.push_back(value);
    }
    PdfDictionary tree;
    tree.set("Names", PdfObject{std::move(names)});

    const int treeNumber = appender.allocateObject();
    appender.setObject(treeNumber, PdfObject{std::move(tree)});

    PdfObject catalogObject = appender.currentObject(catalog);
    PdfDictionary* catalogDict = catalogObject.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "catalog 不是字典";
        return result;
    }
    PdfDictionary namesDict;
    if (const PdfObject* existing = catalogDict->find("Names")) {
        const PdfObject resolved = appender.source().resolve(*existing);
        if (const PdfDictionary* dict = resolved.asDictionary()) namesDict = *dict;
    }
    namesDict.set("EmbeddedFiles", PdfObject{PdfRef{treeNumber, 0}});
    catalogDict->set("Names", PdfObject{std::move(namesDict)});
    if (!appender.updateObject(catalog, std::move(catalogObject))) {
        result.diagnostic = "無法更新 catalog";
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::attachments
