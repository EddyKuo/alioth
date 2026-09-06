#pragma once

// 頁碼範圍字串的解析（PRD-PAGE-002 的刪除／擷取、PRD-PAGE-004 的戳記範圍）。
//
// 「1,3,5-8」→ {0,2,4,5,6,7}。使用者輸入的是 1 起算的頁碼，程式內部一律
// 0 起算，換算只在這裡做一次。
//
// 三條規則，每一條都是為了擋掉一種「安靜地做錯事」：
//
//   1. **一個壞掉的片段就整串作廢**，不是跳過它繼續。跳過的話「1,x,5」會
//      安靜地刪掉第 1 與第 5 頁，而使用者以為自己輸入的是三段——「部分成功」
//      在刪除操作上比直接報錯危險得多。
//
//   2. **去重**。「3,3」幾乎一定是打字重複，而重複的頁碼會讓刪除的數量
//      與使用者的預期對不上。
//
//   3. **超出文件頁數即作廢**，不是夾住。夾住的話「1-999」會變成「全部」，
//      而那與使用者輸入的意思完全不同。
//
// 純函數，不依賴 Qt——因此可以在沒有 GUI 的情況下密集測試邊界條件，
// 而邊界條件正是這種解析器唯一會出錯的地方。

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace alioth::domain {

// totalPages 為文件的總頁數。回傳 0 起算、已排序去重的頁碼；
// 輸入不合法或超出範圍時回傳空 vector。
//
// 空字串回傳空 vector：呼叫端要自己決定「沒輸入」代表全部還是代表取消，
// 兩種語意在不同的操作上都成立（戳記＝全部，刪除＝取消）。
[[nodiscard]] inline std::vector<int> parsePageRange(std::string_view text, int totalPages) {
    std::vector<int> pages;
    if (totalPages <= 0) return pages;

    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t stop = comma == std::string_view::npos ? text.size() : comma;
        std::string_view token = text.substr(cursor, stop - cursor);
        cursor = stop + 1;

        // 兩端修白。使用者常常打成「1, 3 , 5」。
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
            token.remove_prefix(1);
        }
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
            token.remove_suffix(1);
        }
        if (token.empty()) {
            if (comma == std::string_view::npos) break;
            continue;  // 「1,,3」的空片段忽略即可，它不是錯誤只是多打了一個逗號
        }

        const auto parseNumber = [](std::string_view part, bool& ok) {
            ok = !part.empty();
            int value = 0;
            for (const char c : part) {
                if (c < '0' || c > '9') {
                    ok = false;
                    return 0;
                }
                value = value * 10 + (c - '0');
                if (value > 1'000'000'000) {  // 溢位防護：頁碼不可能這麼大
                    ok = false;
                    return 0;
                }
            }
            return value;
        };

        const std::size_t dash = token.find('-');
        bool okFirst = false;
        bool okLast = false;
        int first = 0;
        int last = 0;
        if (dash != std::string_view::npos && dash > 0) {
            std::string_view lhs = token.substr(0, dash);
            std::string_view rhs = token.substr(dash + 1);
            while (!lhs.empty() && std::isspace(static_cast<unsigned char>(lhs.back()))) {
                lhs.remove_suffix(1);
            }
            while (!rhs.empty() && std::isspace(static_cast<unsigned char>(rhs.front()))) {
                rhs.remove_prefix(1);
            }
            first = parseNumber(lhs, okFirst);
            last = parseNumber(rhs, okLast);
        } else {
            first = parseNumber(token, okFirst);
            last = first;
            okLast = okFirst;
        }
        if (!okFirst || !okLast) return {};

        // 「8-5」視為「5-8」。使用者的意思很明確，沒有理由為此報錯。
        if (first > last) std::swap(first, last);
        if (first < 1 || last > totalPages) return {};
        for (int page = first; page <= last; ++page) pages.push_back(page - 1);

        if (comma == std::string_view::npos) break;
    }

    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

}  // namespace alioth::domain
