#include "table_extraction.h"

#include <algorithm>
#include <cmath>

namespace alioth::domain {
namespace {

// 與 engine/text/text_extractor.cpp 的 kSameLineOverlapRatio 同一個數字、
// 同一個理由：兩個字元外框的垂直重疊需達較矮者的一半才算同一視覺行。
// 兩處各自維護一份常數而不是共用標頭，是因為前者屬引擎轉接層（可以碰 PDFium
// 的字元外框型別），這裡是純領域層；為同一顆常數建一個跨層共用標頭不值得，
// 但語意必須保持一致，所以此處重複寫明理由而不是憑空取一個新數字。
constexpr double kSameLineOverlapRatio = 0.5;

// 同一列內，兩個字元的水平間距超過「列高的這個倍數」就視為換詞（不同儲存格
// 或至少不同詞）。取字高而非固定點數，是因為表格的字級不一定與內文相同，
// 用絕對值會讓小字級的表格把整列誤判成一個詞、大字級的表格把一個詞拆散。
constexpr double kWordGapRatio = 0.35;

struct Row {
    RectF box{};
    std::vector<const TextChar*> chars;  // 依 left 由小到大排序（建構後）
};

struct Token {
    std::string text;
    double left{0.0};
    double right{0.0};
};

bool isTableContentChar(const TextChar& ch) noexcept {
    const CharCategory category = ch.category();
    if (category == CharCategory::Control || category == CharCategory::Whitespace) return false;
    return !ch.box.isEmpty();
}

// 把選取範圍內的字元分群成列。回傳的列已依 top 由大到小排序（由上而下的閱讀順序）。
std::vector<Row> clusterRows(const std::vector<const TextChar*>& chars) {
    std::vector<Row> rows;
    for (const TextChar* ch : chars) {
        Row* best = nullptr;
        double bestRatio = -1.0;
        for (Row& row : rows) {
            const double overlap =
                std::min(row.box.top, ch->box.top) - std::max(row.box.bottom, ch->box.bottom);
            const double minHeight = std::min(row.box.height(), ch->box.height());
            if (minHeight <= 0.0) continue;
            const double ratio = overlap / minHeight;
            if (ratio >= kSameLineOverlapRatio && ratio > bestRatio) {
                bestRatio = ratio;
                best = &row;
            }
        }
        if (best != nullptr) {
            best->chars.push_back(ch);
            best->box = best->box.united(ch->box);
        } else {
            Row row;
            row.box = ch->box;
            row.chars.push_back(ch);
            rows.push_back(std::move(row));
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.box.top > b.box.top; });
    for (Row& row : rows) {
        std::sort(row.chars.begin(), row.chars.end(),
                  [](const TextChar* a, const TextChar* b) { return a->box.left < b->box.left; });
    }
    return rows;
}

// 把一列內排序好的字元合併成詞。
std::vector<Token> tokenizeRow(const Row& row) {
    std::vector<Token> tokens;
    if (row.chars.empty()) return tokens;

    double heightSum = 0.0;
    for (const TextChar* ch : row.chars) heightSum += ch->box.height();
    const double avgHeight = heightSum / static_cast<double>(row.chars.size());
    const double gapThreshold = avgHeight * kWordGapRatio;

    for (const TextChar* ch : row.chars) {
        std::string glyph;
        appendUtf8(glyph, ch->unicode);
        if (!tokens.empty() && ch->box.left - tokens.back().right <= gapThreshold) {
            tokens.back().text += glyph;
            tokens.back().right = std::max(tokens.back().right, ch->box.right);
        } else {
            tokens.push_back(Token{std::move(glyph), ch->box.left, ch->box.right});
        }
    }
    return tokens;
}

struct ColumnRun {
    double left{0.0};
    double right{0.0};
};

// 空白欄分隔：把所有列的詞的水平範圍投影到同一軸上取聯集，
// 聯集後彼此不相鄰的區段就是一欄。
std::vector<ColumnRun> projectColumns(const std::vector<std::vector<Token>>& tokensByRow) {
    std::vector<std::pair<double, double>> intervals;
    for (const auto& tokens : tokensByRow) {
        for (const Token& token : tokens) intervals.emplace_back(token.left, token.right);
    }
    std::sort(intervals.begin(), intervals.end());

    std::vector<ColumnRun> columns;
    for (const auto& [left, right] : intervals) {
        if (!columns.empty() && left <= columns.back().right) {
            columns.back().right = std::max(columns.back().right, right);
        } else {
            columns.push_back(ColumnRun{left, right});
        }
    }
    return columns;
}

// 找出 value 落在哪個欄（columns 依 left 排序）。詞的座標必然落在某個欄的
// [left, right] 之內——欄本來就是由這些詞的聯集算出來的，因此用二分搜尋
// 「第一個 right >= value」即可，不需要容差。
std::size_t columnIndexFor(const std::vector<ColumnRun>& columns, double center) {
    std::size_t lo = 0;
    std::size_t hi = columns.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (columns[mid].right < center) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < columns.size() ? lo : columns.size() - 1;
}

}  // namespace

ExtractedTable extractTable(const PageTextLayer& layer, const RectF& area) {
    ExtractedTable result;
    if (area.isEmpty()) return result;

    std::vector<const TextChar*> selected;
    for (const TextChar& ch : layer.chars()) {
        if (!isTableContentChar(ch)) continue;
        if (!ch.box.intersects(area)) continue;
        selected.push_back(&ch);
    }
    if (selected.empty()) return result;

    const std::vector<Row> rows = clusterRows(selected);

    std::vector<std::vector<Token>> tokensByRow;
    tokensByRow.reserve(rows.size());
    for (const Row& row : rows) tokensByRow.push_back(tokenizeRow(row));

    const std::vector<ColumnRun> columns = projectColumns(tokensByRow);
    if (columns.empty()) return result;

    result.rows.assign(rows.size(), std::vector<std::string>(columns.size()));
    result.columnBoundaries.reserve(columns.size());
    for (const ColumnRun& column : columns) result.columnBoundaries.push_back(column.left);

    for (std::size_t r = 0; r < tokensByRow.size(); ++r) {
        // 記錄本列已經填過的欄，若同一欄被填第二次，代表欄位融合發生在這一列
        // （見標頭「跨欄合併儲存格」的說明）：兩個原本該分屬不同欄的詞被投影
        // 判成同一欄。這不是錯誤，是明確回報給呼叫端的已知限制。
        std::vector<bool> filled(columns.size(), false);
        for (const Token& token : tokensByRow[r]) {
            const double center = (token.left + token.right) * 0.5;
            const std::size_t col = columnIndexFor(columns, center);
            if (filled[col]) {
                result.ambiguous = true;
                result.rows[r][col] += ' ';
                result.rows[r][col] += token.text;
            } else {
                filled[col] = true;
                result.rows[r][col] = token.text;
            }
        }
    }

    return result;
}

std::string tableToTsv(const ExtractedTable& table) {
    std::string out;
    for (const std::vector<std::string>& row : table.rows) {
        for (std::size_t col = 0; col < row.size(); ++col) {
            if (col != 0) out += '\t';
            for (const char c : row[col]) {
                out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
            }
        }
        out += '\n';
    }
    return out;
}

std::string extractTsv(const PageTextLayer& layer, const RectF& area) {
    return tableToTsv(extractTable(layer, area));
}

}  // namespace alioth::domain
