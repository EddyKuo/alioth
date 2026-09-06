#pragma once

// 最小 CSV 讀寫（RFC 4180），供量測結果匯出（PRD-ANN-027）等純文字報表使用。
//
// 純 C++，不依賴 Qt：領域層測試不需要事件迴圈，CSV 的逐字元跳脫規則本身
// 也與 GUI 完全無關。呼叫端若要輸出 QString，只需要轉一次編碼。
//
// 跳脫規則只有一條：欄位若含逗號、雙引號或換行（\r 或 \n），整欄以雙引號包住，
// 欄位內的雙引號本身雙寫成兩個。這是 RFC 4180 §2 的規則，也是 Excel／
// LibreOffice 都認得的形式。刻意不支援其他方言（例如分號分隔），
// 量測匯出只需要一種穩定、可回讀的格式。

#include <string>
#include <vector>

namespace alioth::domain {

// 判斷欄位是否需要跳脫：含分隔符、引號或任何一種換行都要。
[[nodiscard]] inline bool csvFieldNeedsQuoting(const std::string& field) noexcept {
    for (const char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') return true;
    }
    return false;
}

// 跳脫單一欄位。不含外層是否要加引號的判斷——呼叫端（csvRow）決定。
[[nodiscard]] inline std::string csvEscapeField(const std::string& field) {
    if (!csvFieldNeedsQuoting(field)) return field;
    std::string out;
    out.reserve(field.size() + 2);
    out += '"';
    for (const char c : field) {
        if (c == '"') out += '"';  // 雙寫跳脫
        out += c;
    }
    out += '"';
    return out;
}

// 組一整列。欄位之間以逗號分隔，列尾不含換行——由呼叫端決定列與列之間怎麼接。
[[nodiscard]] inline std::string csvRow(const std::vector<std::string>& fields) {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) out += ',';
        out += csvEscapeField(fields[i]);
    }
    return out;
}

// 組一整份 CSV：每列以 \r\n 結尾（RFC 4180 建議，Excel 在缺 \r 時偶爾會誤判編碼）。
[[nodiscard]] inline std::string csvDocument(const std::vector<std::vector<std::string>>& rows) {
    std::string out;
    for (const auto& row : rows) {
        out += csvRow(row);
        out += "\r\n";
    }
    return out;
}

// 去掉開頭的 UTF-8 BOM（EF BB BF），若存在的話。
//
// Excel 在「另存新檔 → CSV UTF-8」時一律加這三個位元組；少了這個步驟，
// 表頭的第一欄名稱會被 BOM 汙染（例如 "\xEF\xBB\xBFname"），
// 拿去跟表單欄位名比對永遠對不上，而且看起來就像名稱打錯字，
// 不會有人聯想到是編碼問題。只認 UTF-8 的 BOM：本專案的字串一律是 UTF-8，
// UTF-16 的 BOM 在這個型別裡不可能出現。
[[nodiscard]] inline std::string stripUtf8Bom(const std::string& text) {
    constexpr char kBom[3] = {static_cast<char>(0xEF), static_cast<char>(0xBB),
                              static_cast<char>(0xBF)};
    if (text.size() >= 3 && text[0] == kBom[0] && text[1] == kBom[1] && text[2] == kBom[2]) {
        return text.substr(3);
    }
    return text;
}

// 解析一整份 CSV 回一列列欄位。狀態機直接處理引號內的逗號／換行／雙引號跳脫，
// 不能用先按行再按逗號切割——欄位裡的換行會把一筆資料切成兩筆。
//
// 這是不可信任輸入（使用者可能自行編輯過匯出的 CSV 再匯入），因此格式錯誤
// （例如檔案結尾仍在引號內）一律盡力解析到目前為止的內容，不丟例外。
//
// 開頭的 UTF-8 BOM 會先被去掉（見 stripUtf8Bom）；呼叫端不需要自己處理。
[[nodiscard]] inline std::vector<std::vector<std::string>> parseCsvDocument(const std::string& rawText) {
    const std::string text = stripUtf8Bom(rawText);
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    bool rowHasContent = false;

    auto endField = [&]() {
        row.push_back(field);
        field.clear();
    };
    auto endRow = [&]() {
        endField();
        rows.push_back(row);
        row.clear();
        rowHasContent = false;
    };

    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < n && text[i + 1] == '"') {
                    field += '"';
                    i += 2;
                    continue;
                }
                inQuotes = false;
                ++i;
                continue;
            }
            field += c;
            ++i;
            continue;
        }
        if (c == '"') {
            inQuotes = true;
            rowHasContent = true;
            ++i;
            continue;
        }
        if (c == ',') {
            endField();
            rowHasContent = true;
            ++i;
            continue;
        }
        if (c == '\r') {
            // \r\n 與單獨的 \r 都視為列結束；\r\n 的 \n 在下一輪被跳過。
            endRow();
            ++i;
            if (i < n && text[i] == '\n') ++i;
            continue;
        }
        if (c == '\n') {
            endRow();
            ++i;
            continue;
        }
        field += c;
        rowHasContent = true;
        ++i;
    }
    // 檔案結尾沒有換行時，最後一列（甚至最後一欄）仍要收進去。
    if (rowHasContent || !field.empty() || !row.empty()) endRow();
    return rows;
}

}  // namespace alioth::domain
