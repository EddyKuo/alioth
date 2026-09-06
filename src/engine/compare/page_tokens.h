#pragma once

// 頁面 token 化（PRD-CMP-001）。
//
// 比對的粒度選詞而不是字元，也不是行：
//
//   - 字元層的差異在畫面上是一堆碎片，審閱者看到「改了 37 處」但其實只是換了一個詞。
//   - 行層在 PDF 上根本不可靠：同一段文字換一個字型或改一次邊界就整段重新斷行，
//     每一行都會被判定為變更。行是排版的產物，不是內容的單位。
//
// 每個 token 都帶著它在頁內的字元索引區間，而那套索引與 PageTextLayer 完全相同，
// 因此差異結果可以直接換成 QuadPoints 畫在頁面上，中間不做任何重新編號。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "domain/text_layer.h"
#include "engine/compare/token_diff.h"

namespace alioth::engine::compare {

struct Token {
    domain::TextRange charRange{};  // 頁內字元索引，半開區間
    std::string text;               // UTF-8 原文（保留大小寫，供顯示）
    TokenId id{0};                  // 比對用的 id；ignoreCase 時大小寫已摺疊
};

struct PageTokens {
    std::int32_t pageIndex{0};
    std::vector<Token> tokens;

    // 整頁 token 序列的雜湊。頁面對齊的第一階段用它找「完全相同的頁」，
    // 那些頁是後續局部對齊的錨點。
    std::uint64_t contentHash{0};

    [[nodiscard]] bool empty() const noexcept { return tokens.empty(); }

    [[nodiscard]] std::vector<TokenId> ids() const {
        std::vector<TokenId> out;
        out.reserve(tokens.size());
        for (const Token& t : tokens) out.push_back(t.id);
        return out;
    }
};

// 由已擷取的頁面文字層產生 token。table 在同一次比對中必須是同一份，
// 否則兩份文件的 id 不在同一個號碼空間裡，比出來全是差異。
[[nodiscard]] PageTokens tokenizePage(const domain::PageTextLayer& layer, TokenTable& table,
                                      bool ignoreCase = false);

// 由 UTF-8 純文字產生 token，字元索引即碼點索引。
// 供測試與非 PDF 來源使用，規則與 tokenizePage 完全相同（同一份實作）。
[[nodiscard]] PageTokens tokenizeText(std::int32_t pageIndex, std::string_view utf8,
                                      TokenTable& table, bool ignoreCase = false);

}  // namespace alioth::engine::compare
