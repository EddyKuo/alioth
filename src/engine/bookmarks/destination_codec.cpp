#include "engine/bookmarks/destination_codec.h"

#include <algorithm>
#include <map>
#include <optional>
#include <variant>

#include "domain/text_layer.h"

namespace alioth::engine::bookmarks {

using domain::bookmarks::Destination;
using domain::bookmarks::NamedDestination;
using domain::bookmarks::ZoomType;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfName;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::PdfString;

namespace {

// 名稱樹的走訪深度上限。/Kids 互指的檔案存在，遞迴實作會直接爆堆疊。
constexpr int kMaxNameTreeDepth = 32;

[[nodiscard]] const PdfArray* asArray(const PdfObject& object) noexcept {
    return object.asArray();
}

// 目標陣列裡的 null 代表「沿用目前值」，與 0 是兩件不同的事。
[[nodiscard]] std::optional<double> optionalNumber(const PdfObject& object) {
    if (!object.isNumber()) return std::nullopt;
    return object.asNumber();
}

void appendOptional(PdfArray& array, const std::optional<double>& value) {
    if (value.has_value()) {
        array.push_back(PdfObject{*value});
    } else {
        array.push_back(PdfObject{objects::PdfNull{}});
    }
}

// 名稱樹的一個節點：要嘛有 /Names（葉節點），要嘛有 /Kids（中間節點）。
void collectNameTree(const PdfSourceDocument& source, const PdfObject& node, int depth,
                     std::vector<std::pair<std::string, PdfObject>>& out) {
    if (depth >= kMaxNameTreeDepth) return;
    const PdfObject resolved = source.resolve(node);
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return;

    if (const PdfObject* names = dict->find("Names")) {
        const PdfObject resolvedNames = source.resolve(*names);
        if (const PdfArray* array = asArray(resolvedNames)) {
            for (std::size_t i = 0; i + 1 < array->size(); i += 2) {
                const PdfObject key = source.resolve((*array)[i]);
                const PdfString* keyString = std::get_if<PdfString>(&key.value());
                if (keyString == nullptr) continue;
                out.emplace_back(keyString->bytes, (*array)[i + 1]);
            }
        }
    }
    if (const PdfObject* kids = dict->find("Kids")) {
        const PdfObject resolvedKids = source.resolve(*kids);
        if (const PdfArray* array = asArray(resolvedKids)) {
            for (const PdfObject& kid : *array) collectNameTree(source, kid, depth + 1, out);
        }
    }
}

}  // namespace

PageIndexMap::PageIndexMap(const PdfSourceDocument& source) : pages_(source.pages()) {}

std::int32_t PageIndexMap::indexOf(const PdfRef& ref) const {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].number == ref.number) return static_cast<std::int32_t>(i);
    }
    return -1;
}

bool PageIndexMap::refAt(std::int32_t index, PdfRef& out) const {
    if (index < 0 || static_cast<std::size_t>(index) >= pages_.size()) return false;
    out = pages_[static_cast<std::size_t>(index)];
    return true;
}

std::string decodeTextString(const PdfString& string) {
    const std::string& bytes = string.bytes;
    std::string out;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            char32_t unit =
                static_cast<char32_t>((static_cast<unsigned char>(bytes[i]) << 8) |
                                      static_cast<unsigned char>(bytes[i + 1]));
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
                const char32_t low =
                    static_cast<char32_t>((static_cast<unsigned char>(bytes[i + 2]) << 8) |
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
    // PDFDocEncoding 在 0x20–0x7E 與 0xA0–0xFF 與 Latin-1 相同，差異只在幾個
    // 排版符號；書籤標題不會用到那些字元，因此以 Latin-1 近似而不引入對照表。
    for (const char c : bytes) domain::appendUtf8(out, static_cast<unsigned char>(c));
    return out;
}

int catalogObjectNumber(const PdfSourceDocument& source) {
    const PdfObject* root = source.trailer().find("Root");
    if (root == nullptr || !root->isRef()) return 0;
    return root->asRef().number;
}

bool decodeDestination(const PdfSourceDocument& source, const PageIndexMap& pages,
                       const PdfObject& value, Destination& out, std::string& outName) {
    outName.clear();
    PdfObject resolved = source.resolve(value);

    // 名稱與字串都是命名目標。名稱物件是 1.1 的寫法、字串是 1.2 的寫法，
    // 兩者指向不同的容器（catalog /Dests 與 /Names /Dests），呼叫端要能分辨，
    // 但對書籤而言只是「一個要查表的鍵」。
    if (resolved.isName()) {
        outName = resolved.asName();
        return false;
    }
    if (const PdfString* string = std::get_if<PdfString>(&resolved.value())) {
        outName = string->bytes;
        return false;
    }

    // 動作字典或含 /D 的目標字典。
    if (const PdfDictionary* dict = resolved.asDictionary()) {
        if (const PdfObject* inner = dict->find("D")) {
            return decodeDestination(source, pages, *inner, out, outName);
        }
        return false;
    }

    const PdfArray* array = asArray(resolved);
    if (array == nullptr || array->empty()) return false;

    const PdfObject& target = (*array)[0];
    if (target.isRef()) {
        const std::int32_t index = pages.indexOf(target.asRef());
        if (index < 0) return false;
        out.pageIndex = index;
    } else if (target.isNumber()) {
        // 遠端目標的頁面直接寫數字。本地檔案不該出現，但畸形檔案會，
        // 當成頁索引比整條目標丟掉好。
        out.pageIndex = static_cast<std::int32_t>(target.asInteger());
    } else {
        return false;
    }

    if (array->size() < 2 || !(*array)[1].isName()) {
        out.zoom = ZoomType::Fit;
        return true;
    }
    if (!domain::bookmarks::zoomTypeFromName((*array)[1].asName(), out.zoom)) {
        out.zoom = ZoomType::Fit;
        return true;
    }

    const auto arg = [array](std::size_t i) -> PdfObject {
        return i < array->size() ? (*array)[i] : PdfObject{};
    };

    switch (out.zoom) {
        case ZoomType::XYZ:
            out.left = optionalNumber(arg(2));
            out.top = optionalNumber(arg(3));
            out.zoomFactor = optionalNumber(arg(4));
            // /XYZ 的倍率 0 與 null 同義（沿用目前倍率）。留著 0 會讓下游
            // 算出無限大的縮放。
            if (out.zoomFactor.has_value() && *out.zoomFactor == 0.0) out.zoomFactor.reset();
            break;
        case ZoomType::FitH:
        case ZoomType::FitBH:
            out.top = optionalNumber(arg(2));
            break;
        case ZoomType::FitV:
        case ZoomType::FitBV:
            out.left = optionalNumber(arg(2));
            break;
        case ZoomType::FitR:
            out.left = optionalNumber(arg(2));
            out.bottom = optionalNumber(arg(3));
            out.right = optionalNumber(arg(4));
            out.top = optionalNumber(arg(5));
            break;
        case ZoomType::Fit:
        case ZoomType::FitB:
            break;
    }
    return true;
}

bool encodeDestination(const PageIndexMap& pages, const Destination& destination, PdfObject& out) {
    PdfRef page{};
    if (!pages.refAt(destination.pageIndex, page)) return false;

    PdfArray array;
    array.push_back(PdfObject{page});
    array.push_back(PdfObject{PdfName{domain::bookmarks::zoomTypeName(destination.zoom)}});

    switch (destination.zoom) {
        case ZoomType::XYZ:
            appendOptional(array, destination.left);
            appendOptional(array, destination.top);
            appendOptional(array, destination.zoomFactor);
            break;
        case ZoomType::FitH:
        case ZoomType::FitBH:
            appendOptional(array, destination.top);
            break;
        case ZoomType::FitV:
        case ZoomType::FitBV:
            appendOptional(array, destination.left);
            break;
        case ZoomType::FitR:
            // /FitR 的四個參數全部必填：寫 null 進去的檔案在 Acrobat 會被
            // 當成語法錯誤而不是「沿用目前值」。
            if (!destination.left.has_value() || !destination.bottom.has_value() ||
                !destination.right.has_value() || !destination.top.has_value()) {
                return false;
            }
            array.push_back(PdfObject{*destination.left});
            array.push_back(PdfObject{*destination.bottom});
            array.push_back(PdfObject{*destination.right});
            array.push_back(PdfObject{*destination.top});
            break;
        case ZoomType::Fit:
        case ZoomType::FitB:
            break;
    }

    out = PdfObject{std::move(array)};
    return true;
}

PdfObject makeGoToAction(PdfObject destination) {
    PdfDictionary action;
    action.set("S", objects::makeName("GoTo"));
    action.set("D", std::move(destination));
    return PdfObject{std::move(action)};
}

std::vector<NamedDestinationEntry> readNamedDestinationEntries(const PdfSourceDocument& source,
                                                               const PageIndexMap& pages) {
    std::vector<std::pair<std::string, PdfObject>> raw;

    const int catalogNumber = catalogObjectNumber(source);
    if (catalogNumber <= 0) return {};
    const PdfObject catalog = source.object(catalogNumber);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) return {};

    // 舊式：catalog /Dests 是一個名稱 -> 目標的字典。
    if (const PdfObject* dests = catalogDict->find("Dests")) {
        const PdfObject resolved = source.resolve(*dests);
        if (const PdfDictionary* dict = resolved.asDictionary()) {
            for (const auto& [key, value] : dict->entries()) raw.emplace_back(key, value);
        }
    }
    // 新式：/Names /Dests 名稱樹。後掃因此同名時覆蓋舊式的項目。
    if (const PdfObject* names = catalogDict->find("Names")) {
        const PdfObject resolved = source.resolve(*names);
        if (const PdfDictionary* dict = resolved.asDictionary()) {
            if (const PdfObject* dests = dict->find("Dests")) {
                collectNameTree(source, *dests, 0, raw);
            }
        }
    }

    std::map<std::string, NamedDestinationEntry> merged;
    for (const auto& [name, value] : raw) {
        Destination destination;
        std::string indirect;
        NamedDestinationEntry item;
        item.entry.name = name;
        if (decodeDestination(source, pages, value, destination, indirect)) {
            item.entry.destination = destination;
            item.resolved = true;
        } else if (!indirect.empty()) {
            // 命名目標指向另一個命名目標。這是合法但罕見的間接寫法，
            // 解到底需要再查一次表，而那會引入循環的可能；先當成無法解析。
            item.entry.destination.pageIndex = -1;
            item.resolved = false;
        } else {
            item.entry.destination.pageIndex = -1;
            item.resolved = false;
        }
        merged[name] = std::move(item);
    }

    std::vector<NamedDestinationEntry> result;
    result.reserve(merged.size());
    for (auto& [name, item] : merged) result.push_back(std::move(item));
    return result;
}

std::vector<NamedDestination> readNamedDestinations(const PdfSourceDocument& source,
                                                    const PageIndexMap& pages) {
    std::vector<NamedDestination> result;
    for (const NamedDestinationEntry& item : readNamedDestinationEntries(source, pages)) {
        if (item.resolved) result.push_back(item.entry);
    }
    return result;
}

NamedDestinationWriteResult writeNamedDestinations(objects::IncrementalAppender& appender,
                                                   const PageIndexMap& pages,
                                                   const std::vector<NamedDestination>& destinations,
                                                   bool merge) {
    NamedDestinationWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開檔";
        return result;
    }

    const int catalogNumber = catalogObjectNumber(appender.source());
    if (catalogNumber <= 0) {
        result.diagnostic = "找不到 catalog（trailer 缺少 /Root）";
        return result;
    }

    // 名稱樹的鍵必須唯一且已排序，否則二分搜尋的解析器會找不到後面的項目。
    std::map<std::string, Destination> entries;
    if (merge) {
        for (const NamedDestination& existing : readNamedDestinations(appender.source(), pages)) {
            entries[existing.name] = existing.destination;
        }
    }
    for (const NamedDestination& entry : destinations) {
        if (entry.name.empty()) continue;
        entries[entry.name] = entry.destination;
    }

    PdfArray names;
    for (const auto& [name, destination] : entries) {
        PdfObject encoded;
        if (!encodeDestination(pages, destination, encoded)) continue;
        names.push_back(objects::makeLiteralString(name));
        // 值一律包成 << /D [...] >>：直接放陣列也合法，但包起來之後
        // 同一個物件可以再加 /SD 之類的擴充而不必改動已經寫出的結構。
        PdfDictionary wrapper;
        wrapper.set("D", std::move(encoded));
        names.push_back(PdfObject{std::move(wrapper)});
    }
    result.entryCount = names.size() / 2;

    PdfDictionary tree;
    tree.set("Names", PdfObject{std::move(names)});
    const int treeNumber = appender.allocateObject();
    appender.setObject(treeNumber, PdfObject{std::move(tree)});
    result.namesTreeObject = treeNumber;

    // catalog /Names 可能是間接參照也可能是行內字典，兩種都要處理；
    // 直接覆寫成新字典會把 /JavaScript、/EmbeddedFiles 等既有項目弄丟。
    PdfObject catalog = appender.currentObject(catalogNumber);
    PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "catalog 不是字典";
        return result;
    }

    const PdfObject* namesEntry = catalogDict->find("Names");
    if (namesEntry != nullptr && namesEntry->isRef()) {
        const int namesNumber = namesEntry->asRef().number;
        PdfObject namesObject = appender.currentObject(namesNumber);
        PdfDictionary* namesDict = namesObject.asDictionary();
        if (namesDict == nullptr) {
            result.diagnostic = "catalog /Names 指到的不是字典";
            return result;
        }
        namesDict->set("Dests", objects::makeRef(treeNumber));
        if (!appender.updateObject(namesNumber, namesObject)) {
            result.diagnostic = "無法更新 /Names 物件";
            return result;
        }
    } else {
        PdfDictionary namesDict;
        if (namesEntry != nullptr) {
            if (const PdfDictionary* existing = namesEntry->asDictionary()) namesDict = *existing;
        }
        namesDict.set("Dests", objects::makeRef(treeNumber));
        catalogDict->set("Names", PdfObject{std::move(namesDict)});
        if (!appender.updateObject(catalogNumber, catalog)) {
            result.diagnostic = "無法更新 catalog";
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::bookmarks
