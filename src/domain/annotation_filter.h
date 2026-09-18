#pragma once

// 註解列表的篩選與排序（PRD-ANN-008）。
//
// 純邏輯，不依賴 Qt：讓「500 則註解依日期排序」這種事能在沒有 GUI 的環境下測。
//
// 排序回傳的是**原陣列的索引**而不是複製一份 AnnotationSummary：面板點選某一列
// 之後要跳到那一則註解，需要知道它在原始清單裡的位置。回傳複本的話，面板得再
// 自己想辦法對回去，而那正是「點了第 3 列卻跳到第 7 則」的來源。

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "annotation.h"

namespace alioth::domain {

// PDF 日期字串（ISO 32000-1 §7.9.4）：D:YYYYMMDDHHmmSSOHH'mm'
// 例如 D:20260906143205+08'00'。
//
// 排序**不能**直接比字串。同一個時刻在不同時區寫出來的字串不同，而且
// 「D:」前綴與時區部分都是選填的——有些產生器只寫到分鐘。直接比字串會讓
// 兩份來自不同時區的註解排錯，而那個錯不會有任何徵兆。
//
// 這裡把它化成 UTC 的可比較整數。無法解析時回傳 0，讓那些註解集中排在一端，
// 而不是散在中間造成「排序看起來壞掉」。
[[nodiscard]] inline std::int64_t parsePdfDate(std::string_view text) noexcept {
    if (text.size() >= 2 && text[0] == 'D' && text[1] == ':') text.remove_prefix(2);
    if (text.size() < 4) return 0;

    const auto digits = [&text](std::size_t offset, std::size_t count) -> int {
        if (offset + count > text.size()) return -1;
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[offset + i]);
            if (std::isdigit(c) == 0) return -1;
            value = value * 10 + (c - '0');
        }
        return value;
    };

    const int year = digits(0, 4);
    if (year < 0) return 0;
    // 缺席的欄位依規格取預設值：月與日是 01，時分秒是 00。
    const int month = std::max(1, digits(4, 2));
    const int day = std::max(1, digits(6, 2));
    const int hour = std::max(0, digits(8, 2));
    const int minute = std::max(0, digits(10, 2));
    const int second = std::max(0, digits(12, 2));

    // 化成「自西元 0 年起的秒數」的近似值。只用來排序，不用來顯示，
    // 因此不需要處理閏年與月份長度——同一年同一月內的比較仍然正確，
    // 跨月跨年的比較也仍然單調遞增。
    std::int64_t stamp = static_cast<std::int64_t>(year) * 31622400 +
                         static_cast<std::int64_t>(month) * 2678400 +
                         static_cast<std::int64_t>(day) * 86400 +
                         static_cast<std::int64_t>(hour) * 3600 +
                         static_cast<std::int64_t>(minute) * 60 + second;

    // 時區。O 是 +、- 或 Z；把本地時間換算回 UTC 才能跨時區比較。
    const std::size_t zoneAt = 14;
    if (text.size() > zoneAt) {
        const char sign = text[zoneAt];
        if (sign == '+' || sign == '-') {
            const int offsetHour = std::max(0, digits(zoneAt + 1, 2));
            const int offsetMinute = std::max(0, digits(zoneAt + 4, 2));
            const std::int64_t offset = offsetHour * 3600 + offsetMinute * 60;
            stamp += (sign == '+') ? -offset : offset;
        }
    }
    return stamp;
}

struct AnnotationFilter {
    // 空的欄位代表「不限」。刻意不用 optional：呼叫端是 UI，空字串與
    // 「沒有選」在那裡本來就是同一件事。
    std::int32_t pageIndex{-1};  // -1 代表全部頁面
    std::string author;
    std::string subtype;
    // 內容關鍵字。比對 contents 與 author，不分大小寫。
    std::string text;

    [[nodiscard]] bool isEmpty() const noexcept {
        return pageIndex < 0 && author.empty() && subtype.empty() && text.empty();
    }
};

enum class AnnotationSortKey : std::uint8_t { Page, Author, Type, Date };

[[nodiscard]] inline std::string toLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] inline bool matchesFilter(const AnnotationSummary& annotation,
                                        const AnnotationFilter& filter) {
    if (filter.pageIndex >= 0 && annotation.pageIndex != filter.pageIndex) return false;
    if (!filter.author.empty() && annotation.author != filter.author) return false;
    if (!filter.subtype.empty() && annotation.subtype != filter.subtype) return false;
    if (!filter.text.empty()) {
        const std::string needle = toLowerAscii(filter.text);
        if (toLowerAscii(annotation.contents).find(needle) == std::string::npos &&
            toLowerAscii(annotation.author).find(needle) == std::string::npos) {
            return false;
        }
    }
    return true;
}

// 篩選並排序，回傳原陣列的索引。
//
// 排序一律是穩定的，而且次要鍵永遠是「頁碼、頁內序號」：依作者排序時，
// 同一位作者的註解要維持文件順序，否則每次重新排序的結果都不一樣，
// 使用者會覺得列表在自己跳動。
[[nodiscard]] inline std::vector<std::size_t> filterAndSort(
    const std::vector<AnnotationSummary>& annotations, const AnnotationFilter& filter,
    AnnotationSortKey key, bool ascending) {
    std::vector<std::size_t> indices;
    indices.reserve(annotations.size());
    for (std::size_t i = 0; i < annotations.size(); ++i) {
        if (matchesFilter(annotations[i], filter)) indices.push_back(i);
    }

    const auto documentOrder = [&annotations](std::size_t a, std::size_t b) {
        if (annotations[a].pageIndex != annotations[b].pageIndex) {
            return annotations[a].pageIndex < annotations[b].pageIndex;
        }
        return annotations[a].indexOnPage < annotations[b].indexOnPage;
    };

    std::stable_sort(indices.begin(), indices.end(),
                     [&](std::size_t a, std::size_t b) {
                         bool less = false;
                         bool equal = false;
                         switch (key) {
                             case AnnotationSortKey::Page:
                                 return ascending ? documentOrder(a, b) : documentOrder(b, a);
                             case AnnotationSortKey::Author:
                                 less = annotations[a].author < annotations[b].author;
                                 equal = annotations[a].author == annotations[b].author;
                                 break;
                             case AnnotationSortKey::Type:
                                 less = annotations[a].subtype < annotations[b].subtype;
                                 equal = annotations[a].subtype == annotations[b].subtype;
                                 break;
                             case AnnotationSortKey::Date: {
                                 const std::int64_t ta = parsePdfDate(annotations[a].modified);
                                 const std::int64_t tb = parsePdfDate(annotations[b].modified);
                                 less = ta < tb;
                                 equal = ta == tb;
                                 break;
                             }
                         }
                         if (equal) return documentOrder(a, b);
                         return ascending ? less : !less;
                     });
    return indices;
}

// 文件順序裡的上一則／下一則註解（PDF-XChange 的 Previous / Next Comment）。
//
// 走文件順序而不是清單目前的排序：清單可以依作者或日期排，但「下一則」
// 在使用者心裡是「往文件後面走」。依清單排序的話，連按幾次之後頁碼會跳來跳去，
// 而使用者以為自己在逐頁往下審。
//
// current 超出範圍代表「還沒有選取任何一則」：往後從第一則開始，往前從最後一則。
// 到頭或到尾時回傳 annotations.size()，**不繞回去**——繞回去的話使用者連按到底
// 會不知不覺回到第一則，以為漏看了中間幾則又走一遍。
[[nodiscard]] inline std::size_t adjacentInDocumentOrder(
    const std::vector<AnnotationSummary>& annotations, std::size_t current, int direction) {
    if (annotations.empty() || direction == 0) return annotations.size();

    std::vector<std::size_t> order(annotations.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&annotations](std::size_t a, std::size_t b) {
        if (annotations[a].pageIndex != annotations[b].pageIndex) {
            return annotations[a].pageIndex < annotations[b].pageIndex;
        }
        return annotations[a].indexOnPage < annotations[b].indexOnPage;
    });

    std::size_t position = order.size();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == current) {
            position = i;
            break;
        }
    }

    if (position >= order.size()) return direction > 0 ? order.front() : order.back();
    if (direction > 0) {
        return position + 1 < order.size() ? order[position + 1] : annotations.size();
    }
    return position > 0 ? order[position - 1] : annotations.size();
}

// 清單裡出現過的作者與型別，供 UI 填下拉選單。已排序且去重。
[[nodiscard]] inline std::vector<std::string> distinctAuthors(
    const std::vector<AnnotationSummary>& annotations) {
    std::vector<std::string> out;
    for (const AnnotationSummary& annotation : annotations) {
        if (annotation.author.empty()) continue;
        out.push_back(annotation.author);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

[[nodiscard]] inline std::vector<std::string> distinctSubtypes(
    const std::vector<AnnotationSummary>& annotations) {
    std::vector<std::string> out;
    for (const AnnotationSummary& annotation : annotations) {
        if (annotation.subtype.empty()) continue;
        out.push_back(annotation.subtype);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace alioth::domain
