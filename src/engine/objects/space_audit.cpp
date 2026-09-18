#include "engine/objects/space_audit.h"

#include <algorithm>
#include <set>
#include <unordered_map>

#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::objects {
namespace {

// 一個物件序列化之後占多少位元組。
//
// 量的是序列化結果而不是原檔裡那一段的長度：原檔那一段的邊界要靠掃描
// `endobj` 才知道，而壞檔裡那個字串可能出現在串流內容中間——量錯的後果是
// 某一個物件被算成幾 MB，整份報告就沒有意義了。序列化的結果會與原檔略有
// 差異（空白、數字格式），但那個差異對「誰占最多」這個問題完全不重要。
[[nodiscard]] std::uint64_t serializedSize(const PdfObject& object) {
    std::string out;
    serializeInto(out, object);
    return static_cast<std::uint64_t>(out.size());
}

[[nodiscard]] bool dictHasName(const PdfDictionary& dict, const char* key, const char* name) {
    const PdfObject* value = dict.find(key);
    return value != nullptr && value->isName(name);
}

// 物件自己宣告的型別 → 類別。認不出來回 Other。
[[nodiscard]] SpaceCategory classify(const PdfObject& object) {
    const PdfDictionary* dict = nullptr;
    if (const PdfStream* stream = object.asStream(); stream != nullptr) {
        dict = &stream->dict;
    } else {
        dict = object.asDictionary();
    }
    if (dict == nullptr) return SpaceCategory::Other;

    if (dictHasName(*dict, "Subtype", "Image")) return SpaceCategory::Images;
    if (dictHasName(*dict, "Type", "Font") || dictHasName(*dict, "Type", "FontDescriptor")) {
        return SpaceCategory::Fonts;
    }
    // 字型程式本身是個沒有 /Type 的串流，只能靠它掛在 /FontDescriptor 的哪個鍵
    // 認出來——那要反查。改為看它自己的特徵：/Length1 /Length2 /Length3 是
    // Type1 字型程式專有，/Subtype /CIDFontType0C 與 /OpenType 也是。
    if (dict->find("Length1") != nullptr || dictHasName(*dict, "Subtype", "CIDFontType0C") ||
        dictHasName(*dict, "Subtype", "Type1C") || dictHasName(*dict, "Subtype", "OpenType")) {
        return SpaceCategory::Fonts;
    }
    if (dictHasName(*dict, "Type", "Annot")) return SpaceCategory::Annotations;
    if (dictHasName(*dict, "Type", "Metadata")) return SpaceCategory::Metadata;
    if (dictHasName(*dict, "Type", "Page") || dictHasName(*dict, "Type", "Pages") ||
        dictHasName(*dict, "Type", "Catalog") || dictHasName(*dict, "Type", "StructTreeRoot") ||
        dictHasName(*dict, "Type", "StructElem") || dictHasName(*dict, "Type", "XObject")) {
        // /Type /XObject 但不是影像 → Form XObject，屬於內容。
        if (dictHasName(*dict, "Type", "XObject")) return SpaceCategory::ContentStreams;
        return SpaceCategory::Structure;
    }
    return SpaceCategory::Other;
}

}  // namespace

const char* describe(SpaceCategory category) noexcept {
    switch (category) {
        case SpaceCategory::Images:         return "影像";
        case SpaceCategory::Fonts:          return "字型";
        case SpaceCategory::ContentStreams: return "頁面內容";
        case SpaceCategory::Annotations:    return "註解";
        case SpaceCategory::Metadata:       return "中繼資料";
        case SpaceCategory::Structure:      return "文件結構";
        case SpaceCategory::Other:          return "其他";
    }
    return "其他";
}

SpaceAuditResult auditSpaceUsage(const std::string& sourceBytes) {
    SpaceAuditResult result;

    PdfSourceDocument source;
    std::string diagnostic;
    const SourceStatus status = source.open(sourceBytes, &diagnostic);
    if (status != SourceStatus::Ok) {
        result.diagnostic = status == SourceStatus::Encrypted
                                ? "加密文件尚不支援空間稽核"
                                : (diagnostic.empty() ? "無法解析文件" : diagnostic);
        return result;
    }

    result.fileBytes = static_cast<std::uint64_t>(sourceBytes.size());

    // 頁面 /Contents 指到的串流先標起來：內容串流沒有 /Type，光看它自己
    // 認不出來，但它通常是僅次於影像的第二大宗，歸進「其他」會讓報告失去用處。
    std::set<int> contentStreams;
    for (const PdfRef& page : source.pages()) {
        const PdfObject pageObject = source.object(page.number);
        const PdfDictionary* dict = pageObject.asDictionary();
        if (dict == nullptr) continue;
        const PdfObject* contents = dict->find("Contents");
        if (contents == nullptr) continue;
        if (contents->isRef()) {
            contentStreams.insert(contents->asRef().number);
            continue;
        }
        const PdfObject resolved = source.resolve(*contents);
        if (const PdfArray* array = resolved.asArray(); array != nullptr) {
            for (const PdfObject& item : *array) {
                if (item.isRef()) contentStreams.insert(item.asRef().number);
            }
        }
    }

    std::unordered_map<int, SpaceCategoryUsage> totals;
    for (const int number : source.objectNumbers()) {
        const PdfObject object = source.object(number);
        if (object.isNull()) continue;

        const SpaceCategory category = contentStreams.count(number) != 0
                                           ? SpaceCategory::ContentStreams
                                           : classify(object);
        const std::uint64_t bytes = serializedSize(object);

        SpaceCategoryUsage& usage = totals[static_cast<int>(category)];
        usage.category = category;
        usage.bytes += bytes;
        ++usage.objectCount;
        result.objectBytes += bytes;
    }

    result.categories.reserve(totals.size());
    for (const auto& entry : totals) result.categories.push_back(entry.second);
    // 由多到少：占最多的那一類是使用者唯一會採取行動的對象。
    std::sort(result.categories.begin(), result.categories.end(),
              [](const SpaceCategoryUsage& lhs, const SpaceCategoryUsage& rhs) {
                  if (lhs.bytes != rhs.bytes) return lhs.bytes > rhs.bytes;
                  return static_cast<int>(lhs.category) < static_cast<int>(rhs.category);
              });

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::objects
