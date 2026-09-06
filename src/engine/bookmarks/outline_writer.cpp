#include "engine/bookmarks/outline_writer.h"

#include <algorithm>

#include "engine/bookmarks/destination_codec.h"

namespace alioth::engine::bookmarks {

using domain::bookmarks::Bookmark;
using domain::bookmarks::BookmarkTree;
using domain::bookmarks::TargetEncoding;
using domain::bookmarks::TargetKind;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;

namespace {

// 展平後的一項。用索引而不是指標，因為 vector 會在展平途中成長。
struct FlatItem {
    const Bookmark* node{nullptr};
    int objectNumber{0};
    int parent{-1};  // -1 代表 /Outlines 根
    int prev{-1};
    int next{-1};
    int first{-1};
    int last{-1};
    int visibleDescendants{0};
};

// 可見子孫數：直接子節點都算，展開的子節點再往下累加。
[[nodiscard]] int visibleDescendantCount(const BookmarkTree& children, int depth) {
    if (depth >= domain::bookmarks::kMaxDepth) return 0;
    int total = 0;
    for (const Bookmark& child : children) {
        ++total;
        if (child.open) total += visibleDescendantCount(child.children, depth + 1);
    }
    return total;
}

// 前序展平並串好五條鏈。刻意在這裡一次算完，讓建字典那一步變成純粹的翻譯，
// 不必再回頭找兄弟——那正是逐節點修補會漏改的地方。
void flatten(const BookmarkTree& level, int parent, int depth, std::vector<FlatItem>& items,
             int& firstOut, int& lastOut) {
    if (depth >= domain::bookmarks::kMaxDepth) {
        firstOut = -1;
        lastOut = -1;
        return;
    }
    int previous = -1;
    firstOut = -1;
    lastOut = -1;

    for (const Bookmark& node : level) {
        const int index = static_cast<int>(items.size());
        FlatItem item;
        item.node = &node;
        item.parent = parent;
        item.prev = previous;
        item.visibleDescendants = visibleDescendantCount(node.children, depth);
        items.push_back(item);

        if (previous >= 0) items[static_cast<std::size_t>(previous)].next = index;
        if (firstOut < 0) firstOut = index;
        lastOut = index;
        previous = index;

        int childFirst = -1;
        int childLast = -1;
        flatten(node.children, index, depth + 1, items, childFirst, childLast);
        items[static_cast<std::size_t>(index)].first = childFirst;
        items[static_cast<std::size_t>(index)].last = childLast;
    }
}

}  // namespace

OutlineWriteResult writeOutline(objects::IncrementalAppender& appender, const BookmarkTree& tree) {
    OutlineWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開檔";
        return result;
    }

    const int catalogNumber = catalogObjectNumber(appender.source());
    if (catalogNumber <= 0) {
        result.diagnostic = "找不到 catalog（trailer 缺少 /Root）";
        return result;
    }

    const PageIndexMap pages(appender.source());

    std::vector<FlatItem> items;
    int rootFirst = -1;
    int rootLast = -1;
    flatten(tree, -1, 0, items, rootFirst, rootLast);

    // 先把所有物件編號配完再組字典：/Prev 指向的是前一個兄弟的**編號**，
    // 邊配邊寫的話第一個項目寫出去時還不知道 /Next 是幾號。
    const int outlinesNumber = appender.allocateObject();
    for (FlatItem& item : items) item.objectNumber = appender.allocateObject();

    const auto refOf = [&items](int index) -> PdfObject {
        return objects::makeRef(items[static_cast<std::size_t>(index)].objectNumber);
    };

    for (const FlatItem& item : items) {
        const Bookmark& node = *item.node;
        PdfDictionary dict;
        dict.set("Title", objects::makeTextString(node.title));
        dict.set("Parent", item.parent < 0 ? objects::makeRef(outlinesNumber) : refOf(item.parent));
        if (item.prev >= 0) dict.set("Prev", refOf(item.prev));
        if (item.next >= 0) dict.set("Next", refOf(item.next));
        if (item.first >= 0) dict.set("First", refOf(item.first));
        if (item.last >= 0) dict.set("Last", refOf(item.last));
        if (item.visibleDescendants > 0) {
            dict.set("Count", PdfObject{static_cast<std::int64_t>(
                                  node.open ? item.visibleDescendants : -item.visibleDescendants)});
        }

        PdfObject destination;
        bool haveDestination = false;
        switch (node.target.kind) {
            case TargetKind::None:
                break;
            case TargetKind::Direct:
                haveDestination = encodeDestination(pages, node.target.destination, destination);
                if (!haveDestination) ++result.droppedTargets;
                break;
            case TargetKind::Named:
                // 命名目標一律以字串寫出（1.2 的名稱樹語法）。用名稱物件寫出去
                // 只有 catalog /Dests 字典查得到，而我們的寫入端輸出的是名稱樹。
                destination = objects::makeLiteralString(node.target.name);
                haveDestination = !node.target.name.empty();
                if (!haveDestination) ++result.droppedTargets;
                break;
        }

        if (haveDestination) {
            if (node.target.encoding == TargetEncoding::GoToAction) {
                dict.set("A", makeGoToAction(std::move(destination)));
            } else {
                dict.set("Dest", std::move(destination));
            }
        }

        if (node.hasColor) {
            dict.set("C", objects::makeNumberArray({node.colorR, node.colorG, node.colorB}));
        }
        // /F 的位元：1 = 斜體、2 = 粗體（§12.3.3 表 153）。
        const int flags = (node.italic ? 1 : 0) | (node.bold ? 2 : 0);
        if (flags != 0) dict.set("F", PdfObject{static_cast<std::int64_t>(flags)});

        appender.setObject(item.objectNumber, PdfObject{std::move(dict)});
        result.itemObjects.push_back(item.objectNumber);
    }

    PdfDictionary outlines;
    outlines.set("Type", objects::makeName("Outlines"));
    if (rootFirst >= 0) {
        outlines.set("First", refOf(rootFirst));
        outlines.set("Last", refOf(rootLast));
    }
    // 根的 /Count 一律非負：負值在根層沒有定義，部分檢視器會整棵不顯示。
    int visibleRoots = 0;
    for (const Bookmark& node : tree) {
        ++visibleRoots;
        if (node.open) visibleRoots += visibleDescendantCount(node.children, 0);
    }
    outlines.set("Count", PdfObject{static_cast<std::int64_t>(visibleRoots)});
    appender.setObject(outlinesNumber, PdfObject{std::move(outlines)});

    PdfObject catalog = appender.currentObject(catalogNumber);
    PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "catalog 不是字典";
        return result;
    }
    catalogDict->set("Outlines", objects::makeRef(outlinesNumber));
    if (!appender.updateObject(catalogNumber, catalog)) {
        result.diagnostic = "無法更新 catalog";
        return result;
    }

    result.ok = true;
    result.outlinesObject = outlinesNumber;
    result.itemCount = items.size();
    return result;
}

}  // namespace alioth::engine::bookmarks
