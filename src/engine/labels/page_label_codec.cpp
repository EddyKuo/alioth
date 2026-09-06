#include "engine/labels/page_label_codec.h"

#include <algorithm>
#include <variant>

namespace alioth::engine::labels {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfName;
using objects::PdfObject;
using objects::PdfSourceDocument;
using objects::PdfString;

// 數字樹與名稱樹同構，只是鍵是整數。深度上限與命名目標那邊一致：
// 惡意或損壞的檔案可以把 /Kids 接成環，沒有上限就是無窮遞迴。
constexpr int kMaxNumberTreeDepth = 32;

const PdfArray* asArray(const PdfObject& object) { return object.asArray(); }

void collectNumberTree(const PdfSourceDocument& source, const PdfObject& node, int depth,
                       std::vector<std::pair<std::int32_t, PdfObject>>& out) {
    if (depth >= kMaxNumberTreeDepth) return;
    const PdfObject resolved = source.resolve(node);
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return;

    if (const PdfObject* nums = dict->find("Nums")) {
        const PdfObject resolvedNums = source.resolve(*nums);
        if (const PdfArray* array = asArray(resolvedNums)) {
            for (std::size_t i = 0; i + 1 < array->size(); i += 2) {
                const PdfObject key = source.resolve((*array)[i]);
                if (!key.isNumber()) continue;
                out.emplace_back(static_cast<std::int32_t>(key.asInteger()), (*array)[i + 1]);
            }
        }
    }
    if (const PdfObject* kids = dict->find("Kids")) {
        const PdfObject resolvedKids = source.resolve(*kids);
        if (const PdfArray* array = asArray(resolvedKids)) {
            for (const PdfObject& kid : *array) collectNumberTree(source, kid, depth + 1, out);
        }
    }
}

int catalogNumber(const PdfSourceDocument& source) {
    const PdfObject* root = source.trailer().find("Root");
    if (root == nullptr || !root->isRef()) return 0;
    return root->asRef().number;
}

}  // namespace

domain::PageLabelMap readPageLabels(const PdfSourceDocument& source) {
    const int catalog = catalogNumber(source);
    if (catalog <= 0) return {};
    const PdfObject catalogObject = source.object(catalog);
    const PdfDictionary* catalogDict = catalogObject.asDictionary();
    if (catalogDict == nullptr) return {};

    const PdfObject* labels = catalogDict->find("PageLabels");
    if (labels == nullptr) return {};

    std::vector<std::pair<std::int32_t, PdfObject>> raw;
    collectNumberTree(source, *labels, 0, raw);

    std::vector<domain::PageLabelRange> ranges;
    ranges.reserve(raw.size());
    for (const auto& [startPage, value] : raw) {
        const PdfObject resolved = source.resolve(value);
        const PdfDictionary* dict = resolved.asDictionary();
        if (dict == nullptr) continue;

        domain::PageLabelRange range;
        range.startPageIndex = startPage;

        // /S 缺席是合法的，代表這一段只有前綴沒有編號。把它當成十進位
        // 會讓每一頁都多出一個不存在的數字。
        range.style = domain::PageLabelStyle::None;
        if (const PdfObject* style = dict->find("S")) {
            const PdfObject resolvedStyle = source.resolve(*style);
            if (resolvedStyle.isName()) {
                if (const auto parsed =
                        domain::pageLabelStyleFromName(resolvedStyle.asName())) {
                    range.style = *parsed;
                }
                // 認不得的樣式名稱維持 None：猜成十進位會產生錯誤的頁碼，
                // 而錯的頁碼比沒有頁碼難察覺。
            }
        }
        if (const PdfObject* prefix = dict->find("P")) {
            const PdfObject resolvedPrefix = source.resolve(*prefix);
            if (const PdfString* text = std::get_if<PdfString>(&resolvedPrefix.value())) {
                range.prefix = text->bytes;
            }
        }
        if (const PdfObject* start = dict->find("St")) {
            const PdfObject resolvedStart = source.resolve(*start);
            if (resolvedStart.isNumber()) {
                range.startNumber = static_cast<std::int32_t>(resolvedStart.asInteger());
            }
        }
        // /St 小於 1 是壞資料。夾成 1 而不是丟掉整段：起始編號錯了頂多頁碼偏移，
        // 丟掉整段會讓後面所有頁面的標籤都跟著換一種樣式。
        if (range.startNumber < 1) range.startNumber = 1;
        if (range.startPageIndex < 0) continue;

        ranges.push_back(std::move(range));
    }

    domain::PageLabelMap map;
    map.setRanges(std::move(ranges));
    return map;
}

PageLabelWriteResult writePageLabels(objects::IncrementalAppender& appender,
                                     const domain::PageLabelMap& labels) {
    PageLabelWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開啟";
        return result;
    }

    const int catalog = catalogNumber(appender.source());
    if (catalog <= 0) {
        result.diagnostic = "找不到 catalog";
        return result;
    }

    PdfArray nums;
    for (const domain::PageLabelRange& range : labels.ranges()) {
        PdfDictionary entry;
        if (range.style != domain::PageLabelStyle::None) {
            entry.set("S", PdfObject{PdfName{domain::pageLabelStyleName(range.style)}});
        }
        if (!range.prefix.empty()) {
            entry.set("P", PdfObject{PdfString{range.prefix, false}});
        }
        // /St 為 1 時省略：那是預設值，寫出來只會讓輸出變大。
        if (range.startNumber != 1) {
            entry.set("St", PdfObject{static_cast<std::int64_t>(range.startNumber)});
        }
        nums.push_back(PdfObject{static_cast<std::int64_t>(range.startPageIndex)});
        nums.push_back(PdfObject{std::move(entry)});
    }

    PdfDictionary tree;
    tree.set("Nums", PdfObject{std::move(nums)});

    const int treeNumber = appender.allocateObject();
    appender.setObject(treeNumber, PdfObject{std::move(tree)});

    // 讀 currentObject 而不是 source().object：同一次附加裡若別的功能也改過
    // catalog（例如剛寫入的命名目標），直接讀原檔會把那個改動抹掉。
    PdfObject catalogObject = appender.currentObject(catalog);
    PdfDictionary* catalogDict = catalogObject.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "catalog 不是字典";
        return result;
    }
    catalogDict->set("PageLabels", PdfObject{objects::PdfRef{treeNumber, 0}});
    if (!appender.updateObject(catalog, std::move(catalogObject))) {
        result.diagnostic = "無法更新 catalog";
        return result;
    }

    result.ok = true;
    result.rangeCount = labels.ranges().size();
    return result;
}

}  // namespace alioth::engine::labels
