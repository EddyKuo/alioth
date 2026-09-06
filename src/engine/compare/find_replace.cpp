#include "engine/compare/find_replace.h"

#include <algorithm>
#include <map>
#include <utility>
#include <variant>

#include "domain/text_layer.h"
#include "engine/compare/utf8_scan.h"

namespace alioth::engine::compare {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::PdfString;

[[nodiscard]] const PdfString* asString(const PdfObject& object) noexcept {
    return std::get_if<PdfString>(&object.value());
}

// PDF 文字字串 → UTF-8。
//
// 兩種編碼：帶 BOM 的 UTF-16BE，以及 PDFDocEncoding。後者在 0x20–0x7E 與 0xA0–0xFF
// 區間和 Latin-1 相同，差異只在 0x18–0x1F 那幾個排版符號；那些字元不會出現在
// 使用者輸入的註解內文裡，所以這裡以 Latin-1 近似而不引入一張對照表。
[[nodiscard]] std::string decodeTextString(const PdfString& string) {
    const std::string& bytes = string.bytes;
    std::string out;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            char32_t unit = static_cast<char32_t>(
                (static_cast<unsigned char>(bytes[i]) << 8) |
                static_cast<unsigned char>(bytes[i + 1]));
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
                const char32_t low = static_cast<char32_t>(
                    (static_cast<unsigned char>(bytes[i + 2]) << 8) |
                    static_cast<unsigned char>(bytes[i + 3]));
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            domain::appendUtf8(out, unit);
        }
        return out;
    }
    for (const char c : bytes) {
        domain::appendUtf8(out, static_cast<unsigned char>(c));
    }
    return out;
}

[[nodiscard]] std::string dictTextString(const PdfSourceDocument& source, const PdfDictionary& dict,
                                         const char* key) {
    const PdfObject* raw = dict.find(key);
    if (raw == nullptr) return {};
    const PdfObject resolved = source.resolve(*raw);
    const PdfString* string = asString(resolved);
    return string == nullptr ? std::string{} : decodeTextString(*string);
}

// 沿 /Parent 鏈往上找欄位屬性。深度上限是必要的：不可信任輸入可以讓 /Parent 互指。
[[nodiscard]] PdfObject inheritedFieldValue(const PdfSourceDocument& source, PdfObject field,
                                            const char* key, int* holderObject) {
    constexpr int kMaxDepth = 32;
    for (int depth = 0; depth < kMaxDepth; ++depth) {
        const PdfDictionary* dict = field.asDictionary();
        if (dict == nullptr) break;
        if (const PdfObject* found = dict->find(key)) {
            return source.resolve(*found);
        }
        const PdfObject* parent = dict->find("Parent");
        if (parent == nullptr || !parent->isRef()) break;
        const PdfRef ref = parent->asRef();
        if (!ref.valid()) break;
        if (holderObject != nullptr) *holderObject = ref.number;
        field = source.object(ref.number);
    }
    return PdfObject{};
}

// 持有 /V 的物件編號。表單的值可能寫在父欄位上，改錯物件的症狀是
// 檢視器仍然顯示舊值——看起來像取代沒生效，實際是寫到了被遮蔽的位置。
[[nodiscard]] int valueHolder(const PdfSourceDocument& source, int fieldObject) {
    constexpr int kMaxDepth = 32;
    int current = fieldObject;
    for (int depth = 0; depth < kMaxDepth; ++depth) {
        const PdfObject object = source.object(current);
        const PdfDictionary* dict = object.asDictionary();
        if (dict == nullptr) break;
        if (dict->has("V")) return current;
        const PdfObject* parent = dict->find("Parent");
        if (parent == nullptr || !parent->isRef()) break;
        const PdfRef ref = parent->asRef();
        if (!ref.valid()) break;
        current = ref.number;
    }
    // 沒有任何祖先帶 /V 時就寫在欄位本身：那是規格上的正確位置。
    return fieldObject;
}

struct MatchSpan {
    std::size_t offset{0};
    std::size_t length{0};
};

[[nodiscard]] bool isWordCodepoint(char32_t c) noexcept {
    return domain::categorize(c) == domain::CharCategory::Word;
}

// 全字比對的邊界判定：命中的前後不得是「詞字元」。
// 表意文字視為邊界——CJK 沒有詞界，若把它當詞字元，中文句子裡的關鍵字永遠不算全字。
[[nodiscard]] bool wholeWordAt(const std::string& text, std::size_t offset, std::size_t length) {
    if (offset > 0 && isWordCodepoint(codepointBefore(text, offset))) return false;
    const std::size_t after = offset + length;
    if (after < text.size()) {
        char32_t next = 0;
        if (decodeUtf8(text, after, next) != 0 && isWordCodepoint(next)) return false;
    }
    return true;
}

[[nodiscard]] std::vector<MatchSpan> scanMatches(const std::string& text, const std::string& query,
                                                 const FindOptions& options) {
    std::vector<MatchSpan> spans;
    if (query.empty() || text.empty() || query.size() > text.size()) return spans;

    // ASCII 摺疊不改變位元組長度，所以摺疊後的位移可以直接用在原文上。
    // 換成任何真正的 Unicode 摺疊時，這個假設就不成立了。
    const std::string haystack = options.matchCase ? text : foldAscii(text);
    const std::string needle = options.matchCase ? query : foldAscii(query);

    std::size_t pos = 0;
    while (pos <= haystack.size() - needle.size()) {
        const std::size_t found = haystack.find(needle, pos);
        if (found == std::string::npos) break;
        if (!options.matchWholeWord || wholeWordAt(text, found, needle.size())) {
            spans.push_back(MatchSpan{found, needle.size()});
            pos = found + needle.size();  // 命中不重疊
        } else {
            pos = found + 1;
        }
        if (pos + needle.size() > haystack.size()) break;
    }
    return spans;
}

}  // namespace

std::vector<EditableText> collectEditableText(const PdfSourceDocument& source) {
    std::vector<EditableText> out;
    std::map<int, std::int32_t> widgetPage;

    const std::vector<PdfRef>& pages = source.pages();
    for (std::size_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex) {
        const PdfObject pageObject = source.object(pages[pageIndex].number);
        const PdfDictionary* pageDict = pageObject.asDictionary();
        if (pageDict == nullptr) continue;
        const PdfObject* annotsRaw = pageDict->find("Annots");
        if (annotsRaw == nullptr) continue;
        const PdfObject annots = source.resolve(*annotsRaw);
        const PdfArray* array = annots.asArray();
        if (array == nullptr) continue;

        for (const PdfObject& entry : *array) {
            // 只處理間接參照的註解：直接內嵌在頁面字典裡的註解沒有自己的物件編號，
            // 要改它就得改寫整個頁面物件，那會把同頁其他註解一起重寫。
            if (!entry.isRef()) continue;
            const PdfRef ref = entry.asRef();
            if (!ref.valid()) continue;
            const PdfObject annotObject = source.object(ref.number);
            const PdfDictionary* annotDict = annotObject.asDictionary();
            if (annotDict == nullptr) continue;

            const PdfObject* subtype = annotDict->find("Subtype");
            if (subtype != nullptr && subtype->isName("Widget")) {
                widgetPage.emplace(ref.number, static_cast<std::int32_t>(pageIndex));
                continue;  // 表單欄位由 AcroForm 那一輪處理，避免重複
            }
            if (!annotDict->has("Contents")) continue;

            EditableText item{};
            item.kind = EditableTextKind::AnnotationContents;
            item.pageIndex = static_cast<std::int32_t>(pageIndex);
            item.objectNumber = ref.number;
            item.label = dictTextString(source, *annotDict, "T");
            item.text = dictTextString(source, *annotDict, "Contents");
            out.push_back(std::move(item));
        }
    }

    const PdfObject* rootRaw = source.trailer().find("Root");
    if (rootRaw == nullptr) return out;
    const PdfObject catalog = source.resolve(*rootRaw);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) return out;
    const PdfObject* acroRaw = catalogDict->find("AcroForm");
    if (acroRaw == nullptr) return out;
    const PdfObject acroForm = source.resolve(*acroRaw);
    const PdfDictionary* acroDict = acroForm.asDictionary();
    if (acroDict == nullptr) return out;
    const PdfObject* fieldsRaw = acroDict->find("Fields");
    if (fieldsRaw == nullptr) return out;
    const PdfObject fields = source.resolve(*fieldsRaw);
    const PdfArray* fieldArray = fields.asArray();
    if (fieldArray == nullptr) return out;

    struct Pending {
        int objectNumber{0};
        std::string prefix;
    };
    std::vector<Pending> stack;
    for (auto it = fieldArray->rbegin(); it != fieldArray->rend(); ++it) {
        if (!it->isRef()) continue;
        stack.push_back(Pending{it->asRef().number, {}});
    }

    // 欄位樹以迭代走訪並設上限：/Kids 互指的檔案存在，遞迴實作會直接爆掉（SDD §7）。
    constexpr int kMaxFields = 20000;
    int visited = 0;
    std::vector<int> seen;
    while (!stack.empty() && visited < kMaxFields) {
        const Pending pending = stack.back();
        stack.pop_back();
        ++visited;
        if (std::find(seen.begin(), seen.end(), pending.objectNumber) != seen.end()) continue;
        seen.push_back(pending.objectNumber);

        const PdfObject fieldObject = source.object(pending.objectNumber);
        const PdfDictionary* fieldDict = fieldObject.asDictionary();
        if (fieldDict == nullptr) continue;

        const std::string part = dictTextString(source, *fieldDict, "T");
        std::string fullName = pending.prefix;
        if (!part.empty()) {
            if (!fullName.empty()) fullName.push_back('.');
            fullName += part;
        }

        if (const PdfObject* kidsRaw = fieldDict->find("Kids")) {
            const PdfObject kids = source.resolve(*kidsRaw);
            if (const PdfArray* kidArray = kids.asArray()) {
                for (auto it = kidArray->rbegin(); it != kidArray->rend(); ++it) {
                    if (!it->isRef()) continue;
                    stack.push_back(Pending{it->asRef().number, fullName});
                }
            }
        }

        int holder = pending.objectNumber;
        const PdfObject fieldType =
            inheritedFieldValue(source, fieldObject, "FT", &holder);
        // 只處理文字欄位。核取方塊與下拉選單的 /V 是名稱或選項識別碼，
        // 對它做字串取代會產生一個不存在的選項，欄位從此顯示為空白。
        if (!fieldType.isName("Tx")) continue;

        const int valueObject = valueHolder(source, pending.objectNumber);
        const PdfObject valueRaw = inheritedFieldValue(source, fieldObject, "V", nullptr);
        const PdfString* valueString = asString(valueRaw);

        EditableText item{};
        item.kind = EditableTextKind::FormFieldValue;
        item.objectNumber = valueObject;
        item.label = fullName;
        item.text = valueString == nullptr ? std::string{} : decodeTextString(*valueString);
        const auto pageIt = widgetPage.find(pending.objectNumber);
        item.pageIndex = pageIt == widgetPage.end() ? -1 : pageIt->second;
        out.push_back(std::move(item));
    }

    return out;
}

std::vector<TextMatch> findMatchesIn(const std::string& text, std::size_t targetIndex,
                                     const std::string& query, const FindOptions& options) {
    std::vector<TextMatch> out;
    for (const MatchSpan& span : scanMatches(text, query, options)) {
        out.push_back(TextMatch{targetIndex, span.offset, span.length});
    }
    return out;
}

std::vector<TextMatch> findMatches(std::span<const EditableText> targets, const std::string& query,
                                   const FindOptions& options) {
    std::vector<TextMatch> out;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const std::vector<TextMatch> matches = findMatchesIn(targets[i].text, i, query, options);
        out.insert(out.end(), matches.begin(), matches.end());
    }
    return out;
}

std::string replaceIn(const std::string& text, const std::string& query,
                      const std::string& replacement, const FindOptions& options,
                      std::int32_t* replacements) {
    const std::vector<MatchSpan> spans = scanMatches(text, query, options);
    if (replacements != nullptr) *replacements = static_cast<std::int32_t>(spans.size());
    if (spans.empty()) return text;

    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;
    for (const MatchSpan& span : spans) {
        out.append(text, cursor, span.offset - cursor);
        out += replacement;
        cursor = span.offset + span.length;
    }
    out.append(text, cursor, text.size() - cursor);
    return out;
}

ReplaceResult replaceAll(std::string pdfBytes, const std::string& query,
                         const std::string& replacement, const FindOptions& options) {
    ReplaceResult result{};
    if (query.empty()) {
        result.diagnostic = "查詢字串為空";
        return result;
    }

    objects::IncrementalAppender appender;
    std::string diagnostic;
    const objects::SourceStatus status = appender.open(std::move(pdfBytes), &diagnostic);
    if (status != objects::SourceStatus::Ok) {
        result.diagnostic = objects::describe(status);
        if (!diagnostic.empty()) {
            result.diagnostic += ": ";
            result.diagnostic += diagnostic;
        }
        return result;
    }

    const std::vector<EditableText> targets = collectEditableText(appender.source());
    bool formChanged = false;

    for (const EditableText& target : targets) {
        std::int32_t count = 0;
        const std::string updated =
            replaceIn(target.text, query, replacement, options, &count);
        if (count == 0) continue;

        PdfObject object = appender.currentObject(target.objectNumber);
        PdfDictionary* dict = object.asDictionary();
        if (dict == nullptr) {
            result.diagnostic = "目標物件不是字典";
            return result;
        }
        dict->set(target.kind == EditableTextKind::AnnotationContents ? "Contents" : "V",
                  objects::makeTextString(updated));
        if (!appender.updateObject(target.objectNumber, object)) {
            result.diagnostic = "無法覆寫物件";
            return result;
        }
        result.replacements += count;
        ++result.changedObjects;
        if (target.kind == EditableTextKind::FormFieldValue) formChanged = true;
    }

    if (result.changedObjects == 0) {
        // 沒有命中就不要動檔案。附加一段空的更新會改變位元組長度，
        // 而「取代零處卻產生了新檔案」會讓外部變更偵測與自動儲存互相打架。
        result.ok = true;
        result.bytes = appender.source().bytes();
        return result;
    }

    if (formChanged) {
        result.needAppearances = true;
        const PdfObject* rootRaw = appender.source().trailer().find("Root");
        if (rootRaw != nullptr && rootRaw->isRef()) {
            const int rootNumber = rootRaw->asRef().number;
            PdfObject catalog = appender.currentObject(rootNumber);
            if (PdfDictionary* catalogDict = catalog.asDictionary()) {
                PdfObject* acroRaw = catalogDict->find("AcroForm");
                if (acroRaw != nullptr && acroRaw->isRef()) {
                    const int acroNumber = acroRaw->asRef().number;
                    PdfObject acro = appender.currentObject(acroNumber);
                    if (PdfDictionary* acroDict = acro.asDictionary()) {
                        acroDict->set("NeedAppearances", true);
                        (void)appender.updateObject(acroNumber, acro);
                    }
                } else if (acroRaw != nullptr && acroRaw->asDictionary() != nullptr) {
                    acroRaw->asDictionary()->set("NeedAppearances", true);
                    (void)appender.updateObject(rootNumber, catalog);
                }
            }
        }
    }

    const objects::BuildResult build = appender.build();
    if (!build.ok) {
        result.diagnostic = build.diagnostic;
        return result;
    }
    result.ok = true;
    result.bytes = build.bytes;
    return result;
}

}  // namespace alioth::engine::compare
