#pragma once

// Token 序列差異（PRD-CMP-001，WBS 5.12）。
//
// 這一層刻意只吃兩串 token id，不吃文件：頁面對齊、頁內文字差異、將來的字元層
// 細部差異都是同一個問題的不同粒度，共用同一份實作才不會出現三套邊界條件。
// 它也因此可以完全不碰 PDFium 與 Qt，用純資料測到底。
//
// 演算法選擇：Myers（1986）的 O(ND) 貪婪法，搭配同一篇論文 §4b 的線性空間分治
// （middle snake + 遞迴）。理由有三：
//
//   1. 產出**最小編輯距離**。文件比對的輸出直接畫在使用者眼前，非最小的結果
//      會把「改了一個詞」畫成整段重寫，審閱者得自己再比對一次。
//   2. 記憶體是 O(N+M) 而不是 O(N×M)。兩份 PDF 都是不可信任輸入，
//      樸素 DP 在兩份各二十萬個 token 時就是 160 GB，那不是「比較慢」而是直接死掉。
//   3. 時間 O(ND) 與差異量成正比。文件比對的常見情形是兩份極為相似（D 很小），
//      這正是 Myers 最快的區間。
//
// 沒有選 patience diff：它靠「只出現一次的 token」當錨點，在行層級（原始碼）很有效，
// 但我們的 token 是詞，而詞在文件裡大量重複，錨點會少到整段退回後備演算法。
//
// 資源上限見 DiffLimits。上限不是效能調校而是安全邊界：超過就明確降級或拒絕，
// 不允許「慢慢跑到把記憶體吃光」（SDD §7，PDF 視為不可信任輸入）。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "domain/diff.h"

namespace alioth::engine::compare {

// Token 以整數 id 表示。
//
// 刻意用「登錄表配號」而不是「字串雜湊」：雜湊碰撞在這裡的症狀是兩個不同的詞
// 被判定為相同，結果是差異靜默少報一處——使用者不會知道有東西沒被標出來，
// 這比多報一處嚴重得多。
using TokenId = std::uint32_t;

class TokenTable {
public:
    [[nodiscard]] TokenId intern(std::string_view text);

    // 已配號時回傳 id，否則回傳 kUnknown。用於「查詢但不要污染登錄表」的場合。
    static constexpr TokenId kUnknown = 0xFFFFFFFFu;
    [[nodiscard]] TokenId lookup(std::string_view text) const;

    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

private:
    std::unordered_map<std::string, TokenId> ids_;
};

struct DiffLimits {
    // 單側 token 數上限。超過即拒絕，不嘗試降級——降級後的結果對這種規模的
    // 輸入也沒有參考價值，不如明確告訴呼叫端「這份太大」。
    // 二十萬個詞約當一份 500 頁的文字型文件。
    std::size_t maxTokensPerSide{200000};

    // middle snake 的格點造訪上限。這是時間上限，不是記憶體上限——
    // 記憶體已由線性空間分治鎖在 O(N+M)。超過時該子問題整段報成替換，
    // 結果仍然正確但不是最小編輯，並以 DiffStatus::Degraded 告知。
    std::uint64_t maxCost{20000000};
};

enum class DiffStatus : std::uint8_t {
    Ok,
    Degraded,        // 有子問題超出 maxCost，該段以整段替換表示
    InputTooLarge,   // 超出 maxTokensPerSide，未進行比對
};

[[nodiscard]] const char* describe(DiffStatus status) noexcept;

struct TokenDiff {
    DiffStatus status{DiffStatus::Ok};

    // 完整覆蓋兩側序列的編輯段（含 Equal）。InputTooLarge 時為空。
    std::vector<domain::EditSpan> spans;

    std::uint64_t cost{0};

    [[nodiscard]] bool ok() const noexcept { return status != DiffStatus::InputTooLarge; }

    [[nodiscard]] bool hasChanges() const noexcept {
        for (const domain::EditSpan& s : spans) {
            if (s.kind != domain::DiffKind::Equal) return true;
        }
        return false;
    }

    // 兩側相同 token 的數量，供相似度計算使用。
    [[nodiscard]] std::int32_t commonCount() const noexcept {
        std::int32_t total = 0;
        for (const domain::EditSpan& s : spans) {
            if (s.kind == domain::DiffKind::Equal) total += s.oldSpan.count();
        }
        return total;
    }
};

[[nodiscard]] TokenDiff diffTokens(std::span<const TokenId> oldTokens,
                                   std::span<const TokenId> newTokens,
                                   const DiffLimits& limits = {});

}  // namespace alioth::engine::compare
