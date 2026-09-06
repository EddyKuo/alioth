#include "engine/bookmarks/page_order_writer.h"

#include <algorithm>
#include <vector>

#include "engine/bookmarks/destination_codec.h"

namespace alioth::engine::bookmarks {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;

namespace {

// 可繼承的頁面屬性（ISO 32000-2 表 30）。壓平頁面樹之前必須全部寫死。
constexpr const char* kInheritable[] = {"Resources", "MediaBox", "CropBox", "Rotate"};

}  // namespace

PageReorderResult reorderPages(objects::IncrementalAppender& appender,
                               const std::vector<std::int32_t>& order) {
    PageReorderResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開檔";
        return result;
    }

    const std::vector<PdfRef>& pages = appender.source().pages();
    if (order.size() != pages.size()) {
        result.diagnostic = "頁序長度與頁數不符";
        return result;
    }
    std::vector<bool> seen(pages.size(), false);
    for (const std::int32_t index : order) {
        if (index < 0 || static_cast<std::size_t>(index) >= pages.size()) {
            result.diagnostic = "頁序含越界的索引";
            return result;
        }
        if (seen[static_cast<std::size_t>(index)]) {
            result.diagnostic = "頁序含重複的索引";
            return result;
        }
        seen[static_cast<std::size_t>(index)] = true;
    }

    const int catalogNumber = catalogObjectNumber(appender.source());
    if (catalogNumber <= 0) {
        result.diagnostic = "找不到 catalog（trailer 缺少 /Root）";
        return result;
    }
    PdfObject catalog = appender.currentObject(catalogNumber);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "catalog 不是字典";
        return result;
    }
    const PdfObject* pagesEntry = catalogDict->find("Pages");
    if (pagesEntry == nullptr || !pagesEntry->isRef()) {
        result.diagnostic = "catalog 的 /Pages 不是間接參照";
        return result;
    }
    const int rootNumber = pagesEntry->asRef().number;

    // 先把繼承屬性寫死。順序很重要：一旦根的 /Kids 換掉，
    // inheritedPageAttribute 就再也查不到原本的中間節點了。
    for (const PdfRef& page : pages) {
        PdfObject pageObject = appender.currentObject(page.number);
        PdfDictionary* dict = pageObject.asDictionary();
        if (dict == nullptr) continue;
        bool changed = false;
        for (const char* key : kInheritable) {
            if (dict->has(key)) continue;
            const PdfObject inherited = appender.source().inheritedPageAttribute(page, key);
            if (inherited.isNull()) continue;
            dict->set(key, inherited);
            ++result.attributesMaterialized;
            changed = true;
        }
        dict->set("Parent", objects::makeRef(rootNumber));
        (void)changed;
        if (!appender.updateObject(page.number, pageObject)) {
            result.diagnostic = "無法更新頁面物件";
            return result;
        }
        ++result.pagesRewritten;
    }

    PdfObject rootObject = appender.currentObject(rootNumber);
    PdfDictionary* rootDict = rootObject.asDictionary();
    if (rootDict == nullptr) {
        result.diagnostic = "頁面樹的根不是字典";
        return result;
    }

    PdfArray kids;
    kids.reserve(order.size());
    for (const std::int32_t index : order) {
        kids.push_back(PdfObject{pages[static_cast<std::size_t>(index)]});
    }
    // /Kids 寫成行內陣列而不是沿用原本可能存在的間接參照：壓平之後那個陣列
    // 物件的內容已經完全不同，重用它的編號只會讓兩件事混在一起。
    rootDict->set("Kids", PdfObject{std::move(kids)});
    rootDict->set("Count", PdfObject{static_cast<std::int64_t>(order.size())});
    // 屬性已經寫死在每一頁上，根上留著舊的繼承值只會在日後新增頁面時造成混淆。
    for (const char* key : kInheritable) rootDict->remove(key);

    if (!appender.updateObject(rootNumber, rootObject)) {
        result.diagnostic = "無法更新頁面樹的根";
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::bookmarks
