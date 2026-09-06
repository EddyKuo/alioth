#pragma once

// 書籤樹的寫入（WBS 9，PRD-BM-001）。
//
// PDFium 只有 FPDFBookmark_* 的讀取 API，建立／刪除／改名／搬移一個都沒有，
// 所以整棵 /Outlines 由這裡自己寫成 PDF 物件。做法是「整棵重寫」而不是
// 逐節點修補：書籤項之間有 /Parent /Prev /Next /First /Last 五條互指的鏈，
// 局部修改要同時改動 3～5 個既有物件，任何一條沒改到的後果是書籤面板顯示
// 出無限迴圈或整段消失，而那在寫入當下不會有任何錯誤。整棵重寫仍然是純附加：
// 舊的書籤物件留在原處，只是新的 xref 不再指向它們。
//
// /Count 的正負號是 ISO 32000-2 §12.3.3 的規定：展開的項目寫可見子孫數（正），
// 收合的寫同一個數字的負值。寫錯不會讓檔案壞掉，但 Acrobat 的書籤面板會出現
// 展開箭頭與實際內容對不上的狀況。

#include <cstddef>
#include <string>
#include <vector>

#include "domain/bookmark_ops.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::bookmarks {

struct OutlineWriteResult {
    bool ok{false};
    std::string diagnostic;
    int outlinesObject{0};
    std::size_t itemCount{0};
    std::vector<int> itemObjects;

    // 有目標指向不存在的頁面而被略過。這不是錯誤（批次操作常在頁面被刪之後
    // 才寫回書籤），但必須讓呼叫端看得見，否則使用者只會發現書籤少了目標。
    std::size_t droppedTargets{0};
};

// 整棵覆寫 /Outlines。傳空樹等於刪除所有書籤。
[[nodiscard]] OutlineWriteResult writeOutline(objects::IncrementalAppender& appender,
                                              const domain::bookmarks::BookmarkTree& tree);

}  // namespace alioth::engine::bookmarks
