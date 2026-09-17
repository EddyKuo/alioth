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

// 一次合成多組（PRD-ANN-028 的「並排」版面）。
//
// 並排 = 每一頁與它的摘要頁併成一張。逐對呼叫 mergePages 也做得到，
// 但那是每對一次全檔重寫：100 頁有註解就是重寫 100 次一份可能 100 MB 的檔案，
// 而症狀只是「很慢」，不會有任何人看得出原因。這個入口在同一份開啟的文件裡
// 把所有組都合成完，只重寫一次。
//
// 每一組的合成頁站在該組**最小的原始頁碼**的位置上，所以並排的結果與原文件
// 的頁序一致。組與組之間的來源頁必須互斥。
struct MergeGroup {
    std::vector<int> pages;               // 0 起算的原始頁碼，順序即填格順序
    domain::compose::MergeLayout layout{};  // 每組可以有自己的版面
};

struct MergeGroupsRequest {
    std::vector<MergeGroup> groups;
    bool moveAnnotations{true};
    bool removeSourcePages{true};
};

struct MergeGroupsResult : PageOpsResult {
    std::vector<int> mergedPageIndices;  // 與 groups 的頁序一致（由小到大）
    int movedAnnotations{0};
};

[[nodiscard]] MergeGroupsResult mergePageGroups(std::string sourceBytes,
                                                const MergeGroupsRequest& request);

}  // namespace alioth::engine::pageops
