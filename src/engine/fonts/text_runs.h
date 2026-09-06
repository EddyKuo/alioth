#pragma once

// 把一行文字切成「同一個字型畫得完」的連續段（ADR-007）。
//
// 拉丁與 CJK 在 PDF 裡是兩個不同的字型物件，而且**編碼方式不同**：
// 拉丁走單位元組的 WinAnsi 字面值，CJK 走雙位元組的 Identity-H
// （每個字兩位元組的 glyph index）。整行用同一個字型畫的話，不是中文變亂碼，
// 就是拉丁字被當成雙位元組讀掉——兩種都是「畫出來是別的東西」，
// 而不是明顯的失敗。
//
// 這份邏輯原本在 annotations 與 formbuild 各寫一次。抽出來是因為兩邊只要
// 有一處的跳脫或編碼不一致，就會有一類文字在某一條路徑上壞掉而另一條正常——
// 那種缺陷很難聯想到「兩份實作不同步」。

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace alioth::engine::fonts {

struct TextRun {
    bool cjk{false};
    // cjk 為 true 時已是 Identity-H 的跳脫位元組（可直接放進字串字面值的括號內）；
    // false 時是原始 ASCII，呼叫端仍需自行做 PDF 字串跳脫。
    std::string bytes;
    // 這一段用到的碼點，供呼叫端彙整出子集內容。
    std::set<char32_t> codepoints;
};

// UTF-8 → 碼點序列。非法位元組跳過而不是換成替代字元：替代字元會變成一個
// 使用者沒輸入過的字，而跳過至少不會無中生有。
[[nodiscard]] std::u32string decodeUtf8(const std::string& text);

// 依字型切段。CJK 的位元組已完成 Identity-H 編碼與跳脫。
[[nodiscard]] std::vector<TextRun> splitTextRuns(const std::string& utf8Line);

// 這個碼點是否需要走 CJK 字型（亦即不是 ASCII）。
[[nodiscard]] inline bool needsCjkFont(char32_t codepoint) noexcept { return codepoint >= 128; }

}  // namespace alioth::engine::fonts
