#pragma once

// 頁面對齊（PRD-CMP-001）。
//
// 這是整個比對最容易做錯、也最影響可用性的一步。假設「第 n 頁對第 n 頁」在
// 插入一頁之後就全盤崩壞：使用者看到的不是「插入了一頁」，而是「後面每一頁都被改寫」。
//
// 兩階段：
//
//   1. 錨點：以整頁 token 序列的雜湊當作 token，跑一次 Myers 差異。
//      文字完全相同的頁面因此被一對一釘住，而且釘住的順序是單調的。
//      插入一頁時，插入點之後的頁面雜湊沒有改變，仍然各自對上——這正是第一階段的目的。
//   2. 局部對齊：兩個錨點之間剩下的「舊 i 頁 vs 新 j 頁」再做一次相似度動態規劃，
//      把「內容有小幅修改」的頁面配起來。這一段的規模通常很小，因為錨點已經把
//      整份文件切碎了。
//
// 順序是單調的，不偵測頁面搬移：搬移的頁面會被報成「舊的刪除 + 新的插入」。
// 這是刻意的——並排檢視必須兩邊同步捲動，非單調的對應在畫面上無法呈現。

#include <cstddef>
#include <span>
#include <vector>

#include "domain/diff.h"
#include "engine/compare/page_tokens.h"
#include "engine/compare/token_diff.h"

namespace alioth::engine::compare {

struct AlignOptions {
    // 判定「這兩頁是同一頁的兩個版本」的相似度下限。
    //
    // 0.5 的意思是兩頁至少共用一半的詞（Sørensen–Dice，見 pageSimilarity）。
    // 取這個值有兩個理由：低於一半共用內容時，把它畫成「這頁被改成那樣」比
    // 直接畫成「刪一頁、加一頁」更難讀；而同一份文件的不同頁面往往共用頁首、
    // 頁尾與頁碼，那個雜訊底線在真實文件上約在 0.2–0.3，門檻必須明顯高過它。
    double similarityThreshold{0.5};

    // 局部對齊的相似度比較次數上限。
    //
    // 這是時間上限。單次比較是 O(兩頁 token 數)，二十五萬次比較在一般頁面大小下
    // 是幾百毫秒的量級。超過就退回「同一區塊內依序一對一配」並標記降級——
    // 那個結果在「每一頁都改過」的文件上仍然可用，只是不保證最佳。
    std::size_t maxSimilarityComparisons{250000};

    // 頁數上限。超過即拒絕對齊（不可信任輸入）。
    std::size_t maxPages{10000};
};

struct AlignResult {
    std::vector<domain::PageAlignment> alignments;
    bool degraded{false};
    bool rejected{false};  // 超出 maxPages
};

// 兩頁的文字相似度，Sørensen–Dice 係數（多重集合）：2×共同 token 數 ÷ 兩頁 token 總數。
//
// 用多重集合而不是集合，是因為「同一個詞出現五次」與「出現一次」在文件裡是不同的內容；
// 用 Dice 而不是 Jaccard，是因為 Dice 對「一邊比較短」較寬容，
// 而頁尾那種只有幾行字的頁面正是最需要被正確配對的。
// 兩頁皆無文字時定義為 1.0：掃描件整份都沒有文字層，回 0 會讓它們互相排斥。
[[nodiscard]] double pageSimilarity(const PageTokens& a, const PageTokens& b);

[[nodiscard]] AlignResult alignPages(std::span<const PageTokens> oldPages,
                                     std::span<const PageTokens> newPages,
                                     const AlignOptions& options = {},
                                     const DiffLimits& diffLimits = {});

}  // namespace alioth::engine::compare
