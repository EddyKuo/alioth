#pragma once

// 字數統計（PRD-UI-019，PRD 標記為 C／R2）。
//
// 偏離說明：本工作包（WP30）主要範圍是 R1，PRD-UI-019 屬 R2。這裡只交付純邏輯
// 核心與測試，刻意不接上任何選單／面板／快捷鍵，也不在 M1 前的任何流程中觸發
// ——避免「提前實作 R2 項目」（CLAUDE.md 明文禁止）。之所以仍然把邏輯寫出來，
// 是因為它不吃排程也不佔額外相依，之後要接上 UI 時就是薄殼的事。是否要在
// R1 就開放這個功能是產品範圍決策，不是本次任務可以自行決定的（IL-1）。
//
// 統計只吃既有 alioth_text 擷取出來的純文字（QString），本身不呼叫 PDFium、
// 不認識頁面或引擎——維持「引擎轉接層是唯一可以呼叫 PDFium 的地方」這條線。
//
// 字數與字元數分開統計是刻意的：CJK 沒有空白分字，若照西文的「空白斷詞」邏輯
// 數字數，一整段中文只會被算成一個詞，數字毫無意義。做法是每個 CJK 表意文字
// 算一個詞（比照 Microsoft Word／多數中文編輯器的慣例），西文則以空白與標點
// 斷出的字母數字連續段落算一個詞。

#include <QString>

namespace alioth::app {

struct TextStatistics {
    long long characterCountWithSpaces{0};  // 全部可見字元（含空白，不含換行控制碼）
    long long characterCountNoSpaces{0};    // 不含任何空白字元
    long long wordCount{0};                 // CJK 逐字 + 西文以空白/標點斷詞
    long long cjkCharacterCount{0};          // wordCount 裡有多少來自 CJK（供 UI 分別標示）
    long long latinWordCount{0};             // wordCount 裡有多少來自西文斷詞
};

// 對單一字串（通常是某頁或整份文件的擷取文字）計算統計。純函數，無副作用。
[[nodiscard]] TextStatistics computeTextStatistics(const QString& text);

// 累加多頁的統計（整份文件 = 逐頁相加）。
[[nodiscard]] TextStatistics operator+(const TextStatistics& a, const TextStatistics& b);

}  // namespace alioth::app
