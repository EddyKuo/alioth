#include "engine/objects/reading_order.h"

#include <algorithm>
#include <cstddef>

namespace alioth::engine::objects {
namespace {

struct Collected {
    const StructNode* node{nullptr};
    PageOrderItem item;
};

void collect(const StructNode& node, int depth, std::int32_t pageIndex,
            std::vector<Collected>& out) {
    const bool included = node.pageIndex == pageIndex;
    if (included) {
        Collected entry;
        entry.node = &node;
        entry.item.type = node.type;
        entry.item.title = node.title;
        entry.item.altOrActualText = node.altText.empty() ? node.actualText : node.altText;
        entry.item.depth = depth;
        out.push_back(std::move(entry));
    }
    // 未收錄的祖先（例如 Document、Sect 這類跨頁容器，/Pg 常常不是本頁）不佔一層縮排，
    // 否則每一頁都會憑空多出好幾層空白，UI 上看起來像巢狀很深但其實沒有意義。
    const int childDepth = included ? depth + 1 : depth;
    for (const StructNode& child : node.children) collect(child, childDepth, pageIndex, out);
}

}  // namespace

PageReadingOrder computePageReadingOrder(const StructTree& tree, std::int32_t pageIndex) {
    PageReadingOrder result;
    result.pageIndex = pageIndex;

    std::vector<Collected> collected;
    for (const StructNode& root : tree.roots) collect(root, 0, pageIndex, collected);

    result.items.reserve(collected.size());
    for (std::size_t i = 0; i < collected.size(); ++i) {
        PageOrderItem item = collected[i].item;
        item.structureRank = static_cast<int>(i);
        result.items.push_back(std::move(item));
    }

    // 依 MCID 取得內容順序；沒有 MCID 的節點排除在這條軸之外，不強行湊名次。
    std::vector<std::size_t> withMcid;
    withMcid.reserve(collected.size());
    for (std::size_t i = 0; i < collected.size(); ++i) {
        if (collected[i].node->minMcid() >= 0) withMcid.push_back(i);
    }
    // withMcid 目前依原始索引遞增排列，也就是依結構順序排列（collect 是前序走訪，
    // structureRank 就是索引本身）；穩定排序後才是依內容（MCID）順序排列。
    std::stable_sort(withMcid.begin(), withMcid.end(), [&](std::size_t a, std::size_t b) {
        return collected[a].node->minMcid() < collected[b].node->minMcid();
    });
    for (std::size_t rank = 0; rank < withMcid.size(); ++rank) {
        result.items[withMcid[rank]].contentRank = static_cast<int>(rank);
    }
    result.unknownContentOrderCount = static_cast<int>(collected.size() - withMcid.size());

    // 不一致偵測：把 withMcid 再依索引（=結構順序）排回來看它們各自的 contentRank
    // 是否單調遞增。任何非遞增數列必定存在至少一組相鄰逆序，因此逐一比對相鄰對
    // 就能標出所有不一致發生的位置，不需要另外算「最長遞增子序列」之類更複雜的東西——
    // 這裡要的是「哪裡不對」而不是「最少要動幾個才能對」。
    std::vector<std::size_t> byStructure = withMcid;
    std::sort(byStructure.begin(), byStructure.end());
    for (std::size_t rank = 1; rank < byStructure.size(); ++rank) {
        const int prevContent = result.items[byStructure[rank - 1]].contentRank;
        const int currContent = result.items[byStructure[rank]].contentRank;
        if (currContent < prevContent) {
            result.mismatchIndices.push_back(static_cast<int>(byStructure[rank - 1]));
            result.mismatchIndices.push_back(static_cast<int>(byStructure[rank]));
        }
    }
    std::sort(result.mismatchIndices.begin(), result.mismatchIndices.end());
    result.mismatchIndices.erase(
        std::unique(result.mismatchIndices.begin(), result.mismatchIndices.end()),
        result.mismatchIndices.end());

    return result;
}

}  // namespace alioth::engine::objects
