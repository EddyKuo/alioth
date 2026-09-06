#pragma once

// 依書籤重排頁面（WBS 9，PRD-BM-018）。
//
// 這是 18 條批次功能裡唯一必須動到頁面樹的一條，成本明顯高於其他條，理由是
// 頁面樹可以是任意深度的巢狀結構，而重排會打散原本的分組。
//
// 作法是把頁面樹**壓平**成單層：根 /Pages 的 /Kids 直接列出所有頁面，順序就是
// 新的頁序。壓平會讓中間的 /Pages 節點被孤立，而那些節點可能帶著頁面靠繼承
// 取得的 /Resources /MediaBox /CropBox /Rotate——不先把它們寫死在每一頁上，
// 重排後的頁面會缺字型、尺寸變成預設值，或旋轉方向跑掉。這是本函式最容易
// 被忽略、也最難從結果反推原因的一步。
//
// 仍然是純附加：每一頁與根節點都以新版本寫在檔尾，原檔位元組不變。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::bookmarks {

struct PageReorderResult {
    bool ok{false};
    std::string diagnostic;
    std::size_t pagesRewritten{0};
    std::size_t attributesMaterialized{0};  // 由繼承改成寫死在頁面上的屬性數
};

// order 必須是 0..pageCount-1 的一個排列。不是排列時直接失敗：
// 少一頁或多一頁的結果是資料損失，而 PDF 不會因此打不開，使用者只會發現
// 有幾頁不見了。
[[nodiscard]] PageReorderResult reorderPages(objects::IncrementalAppender& appender,
                                             const std::vector<std::int32_t>& order);

}  // namespace alioth::engine::bookmarks
