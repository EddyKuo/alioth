#include "engine/objects/page_object_editor.h"

namespace alioth::engine::objects {

namespace {

[[nodiscard]] PageEditStatus failure(std::string reason) {
    return PageEditStatus{false, std::move(reason)};
}

}  // namespace

bool pageRefAt(const IncrementalAppender& appender, int index, PdfRef& out) {
    const auto& pages = appender.source().pages();
    if (index < 0 || static_cast<std::size_t>(index) >= pages.size()) return false;
    out = pages[static_cast<std::size_t>(index)];
    return true;
}

PageEditStatus appendToPageArray(IncrementalAppender& appender, const PdfRef& page,
                                 const std::string& key, PdfObject value) {
    const PdfObject pageObject = appender.currentObject(page.number);
    const PdfDictionary* pageDict = pageObject.asDictionary();
    if (pageDict == nullptr) return failure("頁面物件不是字典");

    PdfDictionary updatedPage = *pageDict;
    const PdfObject* existing = pageDict->find(key);

    if (existing == nullptr || existing->isNull()) {
        PdfArray array;
        array.push_back(std::move(value));
        updatedPage.set(key, PdfObject{std::move(array)});
        if (!appender.updateObject(page.number, PdfObject{std::move(updatedPage)})) {
            return failure("頁面物件不存在於原檔");
        }
        return PageEditStatus{true, {}};
    }

    if (const PdfArray* direct = existing->asArray()) {
        PdfArray array = *direct;
        array.push_back(std::move(value));
        updatedPage.set(key, PdfObject{std::move(array)});
        if (!appender.updateObject(page.number, PdfObject{std::move(updatedPage)})) {
            return failure("頁面物件不存在於原檔");
        }
        return PageEditStatus{true, {}};
    }

    if (existing->isRef()) {
        const PdfRef ref = existing->asRef();
        const PdfObject target = appender.currentObject(ref.number);
        if (const PdfArray* array = target.asArray()) {
            // 改寫陣列物件而不是頁面物件：頁面字典通常大得多，
            // 而增量段的大小是 PRD-IO-001 的驗收指標。
            PdfArray updated = *array;
            updated.push_back(std::move(value));
            if (!appender.updateObject(ref.number, PdfObject{std::move(updated)})) {
                return failure("陣列物件不存在於原檔");
            }
            return PageEditStatus{true, {}};
        }
        // 指向單一串流（/Contents 的常見形態）：轉成陣列並保留原本那一份。
        PdfArray array;
        array.emplace_back(ref);
        array.push_back(std::move(value));
        updatedPage.set(key, PdfObject{std::move(array)});
        if (!appender.updateObject(page.number, PdfObject{std::move(updatedPage)})) {
            return failure("頁面物件不存在於原檔");
        }
        return PageEditStatus{true, {}};
    }

    return failure("頁面的 /" + key + " 型別無法附加");
}

PageEditStatus removeFromPageArray(IncrementalAppender& appender, const PdfRef& page,
                                   const std::string& key, int objectNumber) {
    const PdfObject pageObject = appender.currentObject(page.number);
    const PdfDictionary* pageDict = pageObject.asDictionary();
    if (pageDict == nullptr) return failure("頁面物件不是字典");

    const PdfObject* existing = pageDict->find(key);
    if (existing == nullptr || existing->isNull()) {
        // 陣列本來就不存在。刪除一個不存在的項目不是錯誤——重複執行刪除
        // （例如復原之後再重做）必須是安全的。
        return PageEditStatus{true, {}};
    }

    // 只把參照從陣列裡拿掉，**不刪除註解物件本身**。附加式寫入不能刪東西，
    // 而且留著它也讓「復原」只需要把檔案截回原長度就好——那正是本專案
    // 復原機制的基礎（見 app/command_stack.h 的說明）。
    const auto without = [objectNumber](const PdfArray& array) {
        PdfArray result;
        result.reserve(array.size());
        for (const PdfObject& item : array) {
            if (item.isRef() && item.asRef().number == objectNumber) continue;
            result.push_back(item);
        }
        return result;
    };

    if (const PdfArray* direct = existing->asArray()) {
        PdfDictionary updatedPage = *pageDict;
        updatedPage.set(key, PdfObject{without(*direct)});
        if (!appender.updateObject(page.number, PdfObject{std::move(updatedPage)})) {
            return failure("頁面物件不存在於原檔");
        }
        return PageEditStatus{true, {}};
    }

    if (existing->isRef()) {
        const PdfRef ref = existing->asRef();
        const PdfObject target = appender.currentObject(ref.number);
        if (const PdfArray* array = target.asArray()) {
            if (!appender.updateObject(ref.number, PdfObject{without(*array)})) {
                return failure("陣列物件不存在於原檔");
            }
            return PageEditStatus{true, {}};
        }
    }

    return failure("頁面的 /" + key + " 型別無法移除項目");
}

std::vector<int> pageAnnotationRefs(const PdfSourceDocument& source, const PdfRef& page) {
    std::vector<int> refs;
    const PdfObject pageObject = source.object(page.number);
    const PdfDictionary* pageDict = pageObject.asDictionary();
    if (pageDict == nullptr) return refs;

    const PdfObject* annots = pageDict->find("Annots");
    if (annots == nullptr) return refs;

    const PdfArray* array = annots->asArray();
    PdfObject resolved;
    if (array == nullptr && annots->isRef()) {
        resolved = source.object(annots->asRef().number);
        array = resolved.asArray();
    }
    if (array == nullptr) return refs;

    for (const PdfObject& item : *array) {
        // 直接物件（少見但合法）沒有編號，無法以編號刪除。跳過而不是塞一個
        // 假編號進去——否則序號會與列表對不上，刪錯註解。
        if (item.isRef()) refs.push_back(item.asRef().number);
    }
    return refs;
}

PageEditStatus setPageResource(IncrementalAppender& appender, const PdfRef& page,
                               const std::string& category, const std::string& resourceName,
                               PdfObject value) {
    const PdfSourceDocument& source = appender.source();
    const PdfObject pageObject = appender.currentObject(page.number);
    const PdfDictionary* pageDict = pageObject.asDictionary();
    if (pageDict == nullptr) return failure("頁面物件不是字典");

    const PdfObject* resourcesEntry = pageDict->find("Resources");

    // 情況一：/Resources 是間接參照。直接改寫那個物件，頁面字典不必動。
    if (resourcesEntry != nullptr && resourcesEntry->isRef()) {
        const PdfRef ref = resourcesEntry->asRef();
        const PdfObject target = appender.currentObject(ref.number);
        const PdfDictionary* dict = target.asDictionary();
        if (dict == nullptr) return failure("/Resources 參照的不是字典");

        PdfDictionary resources = *dict;
        const PdfObject* categoryEntry = resources.find(category);
        if (categoryEntry != nullptr && categoryEntry->isRef()) {
            const PdfRef categoryRef = categoryEntry->asRef();
            const PdfObject categoryObject = appender.currentObject(categoryRef.number);
            const PdfDictionary* categoryDict = categoryObject.asDictionary();
            if (categoryDict == nullptr) return failure("/" + category + " 參照的不是字典");
            PdfDictionary updated = *categoryDict;
            updated.set(resourceName, std::move(value));
            if (!appender.updateObject(categoryRef.number, PdfObject{std::move(updated)})) {
                return failure("資源類別物件不存在於原檔");
            }
            return PageEditStatus{true, {}};
        }
        PdfDictionary categoryDict;
        if (categoryEntry != nullptr) {
            if (const PdfDictionary* existing = categoryEntry->asDictionary()) categoryDict = *existing;
        }
        categoryDict.set(resourceName, std::move(value));
        resources.set(category, PdfObject{std::move(categoryDict)});
        if (!appender.updateObject(ref.number, PdfObject{std::move(resources)})) {
            return failure("/Resources 物件不存在於原檔");
        }
        return PageEditStatus{true, {}};
    }

    // 情況二：頁面上有直接的 /Resources，或完全沒有而必須沿 /Parent 繼承。
    // 繼承時一定要把上層的字典複製下來，否則新建的 /Resources 會遮蔽它。
    PdfDictionary resources;
    if (resourcesEntry != nullptr) {
        if (const PdfDictionary* existing = resourcesEntry->asDictionary()) resources = *existing;
    } else {
        const PdfObject inherited = source.inheritedPageAttribute(page, "Resources");
        const PdfObject resolved = source.resolve(inherited);
        if (const PdfDictionary* existing = resolved.asDictionary()) resources = *existing;
    }

    PdfDictionary categoryDict;
    if (const PdfObject* categoryEntry = resources.find(category)) {
        const PdfObject resolved = source.resolve(*categoryEntry);
        if (const PdfDictionary* existing = resolved.asDictionary()) categoryDict = *existing;
    }
    categoryDict.set(resourceName, std::move(value));
    resources.set(category, PdfObject{std::move(categoryDict)});

    PdfDictionary updatedPage = *pageDict;
    updatedPage.set("Resources", PdfObject{std::move(resources)});
    if (!appender.updateObject(page.number, PdfObject{std::move(updatedPage)})) {
        return failure("頁面物件不存在於原檔");
    }
    return PageEditStatus{true, {}};
}

PageEditStatus ensurePageTabOrder(IncrementalAppender& appender, const PdfRef& page,
                                  const std::string& tabsValue) {
    const PdfObject pageObject = appender.currentObject(page.number);
    const PdfDictionary* pageDict = pageObject.asDictionary();
    if (pageDict == nullptr) return failure("頁面物件不是字典");

    // 已有 /Tabs 就不覆蓋，理由見標頭註解。
    if (pageDict->find("Tabs") != nullptr) return PageEditStatus{true, {}};

    PdfDictionary updatedPage = *pageDict;
    updatedPage.set("Tabs", makeName(tabsValue));
    if (!appender.updateObject(page.number, PdfObject{std::move(updatedPage)})) {
        return failure("頁面物件不存在於原檔");
    }
    return PageEditStatus{true, {}};
}

}  // namespace alioth::engine::objects
