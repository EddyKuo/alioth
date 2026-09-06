#pragma once

// 頁面範圍解析（PRD-IO-008 的「範圍」）。
//
// 刻意做成不依賴 QPrinter 的純函式：列印對話框、匯出影像、擷取頁面三處都需要
// 同一套語法，各自再寫一份必然會在邊界條件上分岔（空白、倒序、超出頁數）。
//
// 語法沿用使用者對列印對話框的既有預期：逗號分隔、連字號表區間、
// 「-5」表示從第一頁到第五頁、「7-」表示第七頁到最後一頁。
// 頁碼一律 1-based（那是使用者看到的編號），輸出一律 0-based（那是程式用的索引）。

#include <QString>

#include <cstdint>
#include <vector>

namespace alioth::app::print {

enum class PageSubset : std::uint8_t {
    All,
    Odd,
    Even,
};

struct PageRangeResult {
    std::vector<int> pages;  // 0-based，依使用者書寫順序，已去重
    bool valid{true};
    QString diagnostic;  // valid 為 false 時說明原因，不做靜默失敗
};

// 解析範圍字串。空字串代表全部頁面（列印對話框的預設值就是空的）。
//
// 越界的頁碼會被裁到文件範圍內而不是報錯：使用者輸入「1-9999」意思顯然是
// 「印到最後一頁」，為此彈出錯誤只會惹人厭。反之語法錯誤（非數字、區間反向
// 之外的畸形輸入）必須明確失敗，否則會靜默少印頁面——那在法務用途上是事故。
[[nodiscard]] PageRangeResult parsePageRange(const QString& spec, int pageCount,
                                             PageSubset subset = PageSubset::All);

// 反轉頁序，供「反向列印」使用（PRD-PAGE-012 的列印側對應）。
[[nodiscard]] std::vector<int> reversed(std::vector<int> pages);

}  // namespace alioth::app::print
