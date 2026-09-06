#pragma once

// 頁面標籤（PRD-PAGE-013，PDF 32000-1 §12.4.2 /PageLabels）。
//
// 頁面標籤解決的是「文件的第 1 頁不見得叫做 1」：規範與報告的前言用 i、ii、iii，
// 正文才從 1 開始，附錄又跳成 A-1。審閱者引用的是標籤而不是索引，
// 因此跳頁框輸入 "ii" 必須跳到對的地方——這正是 pageForLabel 存在的理由。
//
// 這一層是純邏輯：不碰 Qt、不碰 PDFium、不碰位元組。編解碼在
// engine/labels/，兩者分開的理由是「iii 是第幾頁」的規則完全不需要 PDF。
//
// 字母樣式有一個很容易寫錯的地方：PDF 規格的序列是 A…Z、AA、BB、CC……
// 也就是**重複字母**，不是 26 進位的 AA、AB、AC。照 Excel 欄名的直覺寫，
// 第 27 頁的標籤就會錯，而且要到超過 26 頁的文件才看得出來。

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alioth::domain {

enum class PageLabelStyle {
    None,          // 只有前綴，沒有數字部分
    Decimal,       // /D
    RomanUpper,    // /R
    RomanLower,    // /r
    LettersUpper,  // /A
    LettersLower,  // /a
};

[[nodiscard]] inline const char* pageLabelStyleName(PageLabelStyle style) noexcept {
    switch (style) {
        case PageLabelStyle::Decimal:      return "D";
        case PageLabelStyle::RomanUpper:   return "R";
        case PageLabelStyle::RomanLower:   return "r";
        case PageLabelStyle::LettersUpper: return "A";
        case PageLabelStyle::LettersLower: return "a";
        case PageLabelStyle::None:         break;
    }
    return "";
}

[[nodiscard]] inline std::optional<PageLabelStyle> pageLabelStyleFromName(std::string_view name) {
    if (name == "D") return PageLabelStyle::Decimal;
    if (name == "R") return PageLabelStyle::RomanUpper;
    if (name == "r") return PageLabelStyle::RomanLower;
    if (name == "A") return PageLabelStyle::LettersUpper;
    if (name == "a") return PageLabelStyle::LettersLower;
    return std::nullopt;
}

// 一段編號範圍。範圍的結束由下一段的起點決定（或文件結尾），這是 PDF 的規定，
// 不另存長度——存了就有兩份真相，而它們遲早會不一致。
struct PageLabelRange {
    std::int32_t startPageIndex{0};  // 這段從哪一頁（0 起算）開始
    PageLabelStyle style{PageLabelStyle::Decimal};
    std::string prefix;
    std::int32_t startNumber{1};  // 這段第一頁的編號，PDF 的 /St，必須 >= 1

    [[nodiscard]] bool isValid() const noexcept {
        return startPageIndex >= 0 && startNumber >= 1;
    }
};

namespace detail {

// 只到 3999。超過的話標準羅馬數字沒有寫法（上劃線那套不在 PDF 規格裡），
// 回傳空字串讓呼叫端退回十進位，比輸出一長串 MMMM… 誠實。
[[nodiscard]] inline std::string toRoman(std::int32_t value, bool upper) {
    if (value <= 0 || value > 3999) return {};
    static constexpr int kValues[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static constexpr const char* kUpper[] = {"M",  "CM", "D",  "CD", "C",  "XC", "L",
                                             "XL", "X",  "IX", "V",  "IV", "I"};
    static constexpr const char* kLower[] = {"m",  "cm", "d",  "cd", "c",  "xc", "l",
                                             "xl", "x",  "ix", "v",  "iv", "i"};
    std::string out;
    std::int32_t remaining = value;
    for (std::size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); ++i) {
        while (remaining >= kValues[i]) {
            out += upper ? kUpper[i] : kLower[i];
            remaining -= kValues[i];
        }
    }
    return out;
}

[[nodiscard]] inline std::int32_t fromRoman(std::string_view text) {
    if (text.empty()) return 0;
    const auto digit = [](char c) -> std::int32_t {
        switch (std::toupper(static_cast<unsigned char>(c))) {
            case 'I': return 1;
            case 'V': return 5;
            case 'X': return 10;
            case 'L': return 50;
            case 'C': return 100;
            case 'D': return 500;
            case 'M': return 1000;
            default:  return 0;
        }
    };
    std::int32_t total = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::int32_t current = digit(text[i]);
        if (current == 0) return 0;  // 非羅馬字元，整串作廢
        const std::int32_t next = (i + 1 < text.size()) ? digit(text[i + 1]) : 0;
        total += (current < next) ? -current : current;
    }
    return total;
}

// PDF 規格的字母序列：A…Z、AA…ZZ、AAA…。第 n 個字母重複 ceil(n/26) 次。
[[nodiscard]] inline std::string toLetters(std::int32_t value, bool upper) {
    if (value <= 0) return {};
    const std::int32_t index = (value - 1) % 26;
    const std::int32_t repeat = (value - 1) / 26 + 1;
    const char base = upper ? 'A' : 'a';
    return std::string(static_cast<std::size_t>(repeat), static_cast<char>(base + index));
}

[[nodiscard]] inline std::int32_t fromLetters(std::string_view text) {
    if (text.empty()) return 0;
    const char first = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    if (first < 'A' || first > 'Z') return 0;
    for (const char c : text) {
        if (std::toupper(static_cast<unsigned char>(c)) != first) return 0;  // 必須全是同一個字母
    }
    return static_cast<std::int32_t>(text.size() - 1) * 26 + (first - 'A') + 1;
}

}  // namespace detail

// 單一範圍內的第 offset 頁（0 起算）的標籤。
[[nodiscard]] inline std::string formatPageLabel(const PageLabelRange& range,
                                                 std::int32_t offsetInRange) {
    if (offsetInRange < 0) return range.prefix;
    const std::int32_t number = range.startNumber + offsetInRange;

    std::string body;
    switch (range.style) {
        case PageLabelStyle::None:
            return range.prefix;
        case PageLabelStyle::Decimal:
            body = std::to_string(number);
            break;
        case PageLabelStyle::RomanUpper:
        case PageLabelStyle::RomanLower:
            body = detail::toRoman(number, range.style == PageLabelStyle::RomanUpper);
            // 超過 3999 沒有標準寫法，退回十進位而不是給出無法解讀的字串。
            if (body.empty()) body = std::to_string(number);
            break;
        case PageLabelStyle::LettersUpper:
        case PageLabelStyle::LettersLower:
            body = detail::toLetters(number, range.style == PageLabelStyle::LettersUpper);
            if (body.empty()) body = std::to_string(number);
            break;
    }
    return range.prefix + body;
}

// 整份文件的標籤表。
class PageLabelMap {
public:
    PageLabelMap() = default;
    explicit PageLabelMap(std::vector<PageLabelRange> ranges) { setRanges(std::move(ranges)); }

    // 無效的範圍會被丟掉，重複起點只保留最後一個——PDF 的名稱樹鍵必須唯一，
    // 重複時後者覆蓋前者，這裡照同樣的語意，免得寫回去之後行為改變。
    void setRanges(std::vector<PageLabelRange> ranges) {
        ranges_.clear();
        for (PageLabelRange& range : ranges) {
            if (!range.isValid()) continue;
            const auto it = std::find_if(ranges_.begin(), ranges_.end(),
                                         [&](const PageLabelRange& existing) {
                                             return existing.startPageIndex == range.startPageIndex;
                                         });
            if (it != ranges_.end()) {
                *it = std::move(range);
            } else {
                ranges_.push_back(std::move(range));
            }
        }
        std::sort(ranges_.begin(), ranges_.end(),
                  [](const PageLabelRange& a, const PageLabelRange& b) {
                      return a.startPageIndex < b.startPageIndex;
                  });
    }

    [[nodiscard]] const std::vector<PageLabelRange>& ranges() const noexcept { return ranges_; }
    [[nodiscard]] bool empty() const noexcept { return ranges_.empty(); }

    // 第 pageIndex 頁（0 起算）的標籤。沒有任何範圍涵蓋它時回傳十進位頁碼——
    // PDF 規格要求第一段必須從第 0 頁開始，但真實文件常常違反，
    // 這時退回頁碼比回傳空字串有用得多。
    [[nodiscard]] std::string labelFor(std::int32_t pageIndex) const {
        if (pageIndex < 0) return {};
        const PageLabelRange* range = rangeFor(pageIndex);
        if (range == nullptr) return std::to_string(pageIndex + 1);
        return formatPageLabel(*range, pageIndex - range->startPageIndex);
    }

    [[nodiscard]] const PageLabelRange* rangeFor(std::int32_t pageIndex) const {
        const PageLabelRange* found = nullptr;
        for (const PageLabelRange& range : ranges_) {
            if (range.startPageIndex > pageIndex) break;
            found = &range;
        }
        return found;
    }

    // 標籤 → 頁索引。跳頁框要用。找不到回傳 nullopt，**不要**退回把輸入當頁碼解讀：
    // 那個決定屬於 UI（它才知道使用者打的是標籤還是頁碼）。
    //
    // pageCount 是必要的：範圍的結束由下一段起點決定，最後一段的結束只有文件知道。
    [[nodiscard]] std::optional<std::int32_t> pageForLabel(std::string_view label,
                                                           std::int32_t pageCount) const {
        if (label.empty() || pageCount <= 0) return std::nullopt;
        // 逐段比對而不是逐頁產生標籤再比：一萬頁的文件在跳頁框每打一個字
        // 就要產生一萬個字串，那是看得出來的延遲。
        for (std::size_t i = 0; i < ranges_.size(); ++i) {
            const PageLabelRange& range = ranges_[i];
            const std::int32_t end = (i + 1 < ranges_.size())
                                         ? ranges_[i + 1].startPageIndex
                                         : pageCount;
            if (range.startPageIndex >= end) continue;

            if (label.size() < range.prefix.size()) continue;
            if (label.compare(0, range.prefix.size(), range.prefix) != 0) continue;
            const std::string_view body = label.substr(range.prefix.size());

            std::int32_t number = 0;
            switch (range.style) {
                case PageLabelStyle::None:
                    // 這一段全部同名，只有整段第一頁能被指名。
                    if (body.empty()) return range.startPageIndex;
                    continue;
                case PageLabelStyle::Decimal: {
                    if (body.empty()) continue;
                    if (!std::all_of(body.begin(), body.end(), [](char c) {
                            return c >= '0' && c <= '9';
                        })) {
                        continue;
                    }
                    number = std::stoi(std::string(body));
                    break;
                }
                case PageLabelStyle::RomanUpper:
                case PageLabelStyle::RomanLower:
                    number = detail::fromRoman(body);
                    break;
                case PageLabelStyle::LettersUpper:
                case PageLabelStyle::LettersLower:
                    number = detail::fromLetters(body);
                    break;
            }
            if (number < range.startNumber) continue;
            const std::int32_t page = range.startPageIndex + (number - range.startNumber);
            if (page < end && page < pageCount) return page;
        }
        return std::nullopt;
    }

    // 標籤是否唯一。不唯一時跳頁框會跳到第一個符合的，這件事應該讓使用者知道，
    // 所以編輯對話框需要這個查詢。
    [[nodiscard]] bool hasDuplicateLabels(std::int32_t pageCount) const {
        std::vector<std::string> seen;
        seen.reserve(static_cast<std::size_t>(std::max(pageCount, 0)));
        for (std::int32_t page = 0; page < pageCount; ++page) {
            std::string label = labelFor(page);
            if (std::find(seen.begin(), seen.end(), label) != seen.end()) return true;
            seen.push_back(std::move(label));
        }
        return false;
    }

private:
    std::vector<PageLabelRange> ranges_;
};

}  // namespace alioth::domain
