#pragma once

// 由書籤產生目錄頁（WBS 9，PRD-BM-005）與頁面上的連結（PRD-BM-008）。
//
// 版面完全由 domain::bookmarks::layoutTableOfContents 算好，這一層只負責把
// 座標翻譯成內容串流運算子、建立新的頁面物件、掛上 /Annots 連結。分成兩層的
// 理由是版面規則（斷頁、縮排、引導點）需要密集測試，而那不該每次都開一份 PDF。
//
// 新頁面掛在頁面樹的**根節點**底下，而不是插進中間的 /Pages 節點：根的 /Kids
// 允許混放 /Page 與 /Pages，把新頁掛在根上只需要改動一個既有物件（根 /Pages 的
// /Kids 與 /Count），中間節點一個都不用碰。插進中間節點的話，從該節點到根的
// 每一層 /Count 都要跟著加一，漏掉任何一層的後果是頁數對不上，而多數檢視器
// 會靜默容忍，直到某個工具照著 /Count 讀而少讀了幾頁。
//
// 字型限定標準 14（Helvetica 系列）。非 ASCII 的標題無法以 WinAnsiEncoding
// 輸出，CJK 字型內嵌的授權策略在 CLAUDE.md 仍是待決策項，因此那些字元會被
// 換成問號並在結果裡明確回報——靜默輸出亂碼比缺字更難察覺。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "domain/bookmark_ops.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::bookmarks {

// 目錄頁要放在文件的哪一端。刻意不提供任意頁碼：頁面樹可以是巢狀的，
// 「第 N 頁之後」在根 /Kids 上沒有對應的位置，硬做會在巢狀文件上插錯地方。
enum class TocPlacement {
    Front,
    Back,
};

struct TocBuildOptions {
    domain::bookmarks::TocLayoutOptions layout{};
    TocPlacement placement{TocPlacement::Front};
    bool createLinks{true};        // PRD-BM-008
    std::string baseFont{"Helvetica"};
    std::string headingFont{"Helvetica-Bold"};
    double leaderDotWidthPt{0.0};  // 0 代表由字級推算
};

struct TocBuildResult {
    bool ok{false};
    std::string diagnostic;
    std::vector<int> pageObjects;
    std::size_t lineCount{0};
    std::size_t linkCount{0};

    // 標題含非 ASCII 字元而被換成問號。降級必須讓使用者看得見（SDD §7）。
    bool degradedNonAscii{false};
};

[[nodiscard]] TocBuildResult buildTableOfContents(objects::IncrementalAppender& appender,
                                                  const domain::bookmarks::BookmarkTree& tree,
                                                  const TocBuildOptions& options = {});

// 在既有頁面上加連結（PRD-BM-008 的另一半：目標頁已經存在時）。
struct LinkWriteResult {
    bool ok{false};
    std::string diagnostic;
    std::size_t written{0};
    std::size_t skipped{0};
};

[[nodiscard]] LinkWriteResult writeBookmarkLinks(
    objects::IncrementalAppender& appender,
    const std::vector<domain::bookmarks::BookmarkLink>& links);

}  // namespace alioth::engine::bookmarks
