#pragma once

// Bates 編號（PRD-PAGE-004）。
//
// 這是法務揭示與稽核的證據基礎：同一份卷宗裡每一頁都要有唯一且連續的編號，
// 各方引用時靠它指向同一頁。號碼一旦跳號、重號或補零位數不一致，
// 交叉引用就會指錯頁，整份標記的證據價值即失效。
//
// 因此編號邏輯刻意與繪製、與列印流程完全解耦：它只是一個由序號到字串的
// 純函式，可以逐一列舉驗證，不需要印表機也不需要 PDF。

#include <QString>

#include <cstdint>
#include <vector>

namespace alioth::app::print {

// 只印部分頁時，號碼要跟著「印出來的順序」還是「文件裡的頁次」？
//
// 兩者都有正當用途，而且選錯的後果不對稱：
//   PrintSequence — 這批列印品自成一份連續卷宗（重新揭示、抽印本）。
//   DocumentPage  — 號碼必須與完整文件的頁次一致（補印遺失頁、部分重印）。
// 沒有安全的預設值可言，所以不提供隱含行為，由呼叫端明示。
enum class BatesBasis : std::uint8_t {
    PrintSequence,
    DocumentPage,
};

struct BatesOptions {
    bool enabled{false};
    QString prefix;
    QString suffix;
    long long startNumber{1};
    int digits{6};      // 補零後的最少位數；0 表示不補零
    int increment{1};   // 每頁遞增量，可為負（倒序編號的卷宗確實存在）
    BatesBasis basis{BatesBasis::PrintSequence};
};

// 數字部分的合法上限。位數超過這個值只會產出沒有人讀得懂的字串，
// 而且多半代表呼叫端把「起始號」誤填進了「位數」欄位。
inline constexpr int kMaxBatesDigits = 15;

// 序號 → 完整 Bates 字串。ordinal 依 basis 決定意義：
// PrintSequence 時是該頁在本次列印集合中的 0-based 次序，
// DocumentPage 時是該頁在文件中的 0-based 頁索引。
[[nodiscard]] QString formatBatesNumber(const BatesOptions& options, long long ordinal);

// 只取數字部分，供比對與排序使用（前後綴常含分隔符，直接字串比較會排錯）。
[[nodiscard]] long long batesValueAt(const BatesOptions& options, long long ordinal) noexcept;

// 一次產生整批號碼。存在的理由是驗證：列印前把序列整個攤開，
// 就能在真的送紙之前檢查有無重號或跳號。
[[nodiscard]] std::vector<QString> batesSequence(const BatesOptions& options,
                                                 const std::vector<int>& pages);

// 序列健檢：號碼必須兩兩相異。increment 為 0 時整批會是同一個號碼，
// 那在法務用途上等同沒有編號，必須在送印前擋下來而不是印完才發現。
[[nodiscard]] bool isBatesSequenceUnique(const std::vector<QString>& numbers);

}  // namespace alioth::app::print
