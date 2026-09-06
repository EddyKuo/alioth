#pragma once

// 合併頁面：多頁疊為一頁（PRD-PAGE-007，WBS 12）。
//
// 「疊為一頁」在 PDF 裡不是把內容串流接起來——那會讓來源頁之間的圖形狀態
// 互相污染（見 page_form.h）。正確做法是每一個來源頁包成獨立的 Form XObject，
// 再各自以 cm 矩陣放到格子裡。
//
// 註解一定要跟著搬並套用同一個矩陣。註解幾何與內容串流完全無關，
// 漏掉的話輸出會是「圖縮小了、螢光筆還留在原本的位置」（見 annotation_transform.h）。
//
// 已知的取捨：來源頁若有 /Rotate，內容會被烘進矩陣正確旋轉，但註解的 /AP
// 外觀串流不會跟著轉——外觀的映射規則由檢視器依 /Rect 重算，只保證位置對。
// 這在 2-up 掃描件上看得出來（螢光筆的方向），但比整格倒過來好得多。

#include <string>
#include <vector>

#include "domain/page_compose.h"
#include "engine/pageops/compose_document.h"

namespace alioth::engine::pageops {

struct MergePagesRequest {
    // 0 起算，順序即填格順序。刻意不接受重複頁：同一個註解物件搬兩次會讓
    // 第二次的矩陣疊在第一次的結果上，標記會飛到頁面外。
    std::vector<int> pages;

    domain::compose::MergeLayout layout{};

    bool moveAnnotations{true};

    // 預設把來源頁從頁面樹移除（合併的語意就是「這幾頁變成一頁」）。
    // 設為 false 時來源頁保留，合併頁插在 insertAt。
    bool removeSourcePages{true};

    // 合併頁插入的位置；-1 表示放在第一個來源頁原本的位置。
    int insertAt{-1};
};

struct MergePagesResult : PageOpsResult {
    int mergedPageIndex{0};
    int movedAnnotations{0};
};

[[nodiscard]] MergePagesResult mergePages(std::string sourceBytes,
                                          const MergePagesRequest& request);

}  // namespace alioth::engine::pageops
