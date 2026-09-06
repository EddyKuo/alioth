#pragma once

// 文字層領域模型（PRD-TXT-001 ~ 004、PRD-SRCH-001）。純 C++，與 PDFium 及 Qt 無關。
//
// 座標一律是 PDF 頁面空間（點，原點左下、Y 軸向上），與 geometry.h 的 RectF 一致。
// PDFium 的 CharBox 本來就是這個座標系，所以擷取時不做任何翻轉；
// 要畫到螢幕上時才經 PageTransform 轉換。任何在此之外的 Y 翻轉都是錯的。
//
// 字元索引直接沿用 PDFium 文字層的索引（含它生成的空白與換行字元）。
// 保持 1:1 對應是刻意的：選取範圍、搜尋結果、QuadPoints 三者要能互相換算，
// 中間若有一層自訂重新編號，任何一次不一致都會讓螢光筆標到別的位置。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "geometry.h"
#include "quad_point.h"

namespace alioth::domain {

// 字元分類。決定雙擊選詞的邊界（PRD-TXT-004）。
enum class CharCategory : std::uint8_t {
    Control,      // 換行、定位等控制字元；不參與選取外觀
    Whitespace,
    Word,         // 字母、數字、底線；連續者視為同一個詞
    Ideograph,    // CJK 表意文字；雙擊只取單字，不做斷詞
    Punctuation,
};

[[nodiscard]] constexpr CharCategory categorize(char32_t c) noexcept {
    if (c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' || c < 0x20 || c == 0x7F) {
        return CharCategory::Control;
    }
    if (c == U' ' || c == 0x00A0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) {
        return CharCategory::Whitespace;
    }
    if ((c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') ||
        c == U'_') {
        return CharCategory::Word;
    }
    // 拉丁文擴充與希臘/西里爾字母：視為詞的一部分，否則歐語詞會被雙擊切碎。
    if ((c >= 0x00C0 && c <= 0x024F) || (c >= 0x0370 && c <= 0x04FF)) {
        return CharCategory::Word;
    }
    // CJK 統一表意文字、假名、諺文。
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x2FA1F)) {
        return CharCategory::Ideograph;
    }
    return CharCategory::Punctuation;
}

// 單一字元。box 是頁面座標的字元外框。
struct TextChar {
    std::int32_t index{0};
    char32_t unicode{0};
    RectF box{};
    std::int32_t lineIndex{0};   // 同一視覺行的字元共用；決定 QuadPoints 的切行
    std::int32_t blockIndex{0};  // 段落／欄；多欄版面的閱讀順序依據（PRD-TXT-001）

    [[nodiscard]] CharCategory category() const noexcept { return categorize(unicode); }
};

// 選取範圍，半開區間 [start, end)。
//
// 用半開區間而非「起訖字元」是為了讓空選取（游標）有唯一表示法：start == end。
// 若用閉區間，空選取只能用 end < start 這種反直覺編碼表示。
struct TextRange {
    std::int32_t start{0};
    std::int32_t end{0};

    [[nodiscard]] constexpr std::int32_t count() const noexcept { return end - start; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return end <= start; }
    [[nodiscard]] constexpr bool contains(std::int32_t i) const noexcept {
        return i >= start && i < end;
    }

    // 使用者可以由後往前拖曳，錨點與游標的先後不固定。
    [[nodiscard]] constexpr TextRange normalized() const noexcept {
        return start <= end ? *this : TextRange{end, start};
    }

    [[nodiscard]] constexpr TextRange clamped(std::int32_t charCount) const noexcept {
        const TextRange n = normalized();
        const std::int32_t lo = n.start < 0 ? 0 : n.start;
        const std::int32_t hi = n.end > charCount ? charCount : n.end;
        return TextRange{lo, hi < lo ? lo : hi};
    }

    [[nodiscard]] static constexpr TextRange fromCount(std::int32_t start,
                                                       std::int32_t count) noexcept {
        return TextRange{start, start + count};
    }

    friend constexpr bool operator==(const TextRange&, const TextRange&) = default;
};

// 一頁內的選取。跨頁選取由應用層持有多個本結構（PRD-TXT-002 的跨頁拼接）。
struct TextSelection {
    std::int32_t pageIndex{0};
    TextRange range{};

    [[nodiscard]] bool isEmpty() const noexcept { return range.isEmpty(); }

    friend constexpr bool operator==(const TextSelection&, const TextSelection&) = default;
};

// 搜尋結果。context 是含前後文的 UTF-8 片段，供結果列表顯示（PRD-SRCH-001）。
struct SearchResult {
    std::int32_t pageIndex{0};
    TextRange range{};
    std::string context;
    std::int32_t matchOffset{0};  // 命中在 context 中的位元組位移
    std::int32_t matchLength{0};  // 命中在 context 中的位元組長度
};

// 把碼點附加為 UTF-8。領域層不引入 Qt，字串一律 UTF-8。
inline void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// 一頁的完整字元層。由引擎轉接層填入，之後所有查詢都不再需要 PDFium。
//
// 這層存在的理由是可測試性與快取：選取拖曳每秒會問上百次幾何，
// 每次都回頭問 PDFium 等於把工作丟回單執行緒佇列，捲動就掉幀了。
class PageTextLayer {
public:
    PageTextLayer() = default;
    PageTextLayer(std::int32_t pageIndex, std::vector<TextChar> chars)
        : pageIndex_(pageIndex), chars_(std::move(chars)) {}

    [[nodiscard]] std::int32_t pageIndex() const noexcept { return pageIndex_; }
    [[nodiscard]] const std::vector<TextChar>& chars() const noexcept { return chars_; }
    [[nodiscard]] std::int32_t charCount() const noexcept {
        return static_cast<std::int32_t>(chars_.size());
    }
    [[nodiscard]] bool isEmpty() const noexcept { return chars_.empty(); }
    [[nodiscard]] TextRange fullRange() const noexcept { return TextRange{0, charCount()}; }

    [[nodiscard]] const TextChar* charAt(std::int32_t index) const noexcept {
        if (index < 0 || index >= charCount()) return nullptr;
        return &chars_[static_cast<std::size_t>(index)];
    }

    // 範圍轉純文字（UTF-8）。UTF-16 代理對在此合併回單一碼點。
    [[nodiscard]] std::string text(TextRange range) const {
        const TextRange r = range.clamped(charCount());
        std::string out;
        out.reserve(static_cast<std::size_t>(r.count()) * 2);
        for (std::int32_t i = r.start; i < r.end; ++i) {
            char32_t c = chars_[static_cast<std::size_t>(i)].unicode;
            if (c == 0) continue;
            // 擷取時保留 PDFium 的每個索引，代理對因此是兩個相鄰字元。
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < r.end) {
                const char32_t low = chars_[static_cast<std::size_t>(i) + 1].unicode;
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            appendUtf8(out, c);
        }
        return out;
    }

    // 選取範圍 → QuadPoints。跨行必須切成多個 quad，否則螢光筆會塗滿整段矩形，
    // 把行與行之間、以及首尾行未選到的部分一起蓋掉。
    [[nodiscard]] std::vector<QuadPoint> quads(TextRange range) const {
        const TextRange r = range.clamped(charCount());
        std::vector<QuadPoint> result;
        RectF current{};
        bool hasCurrent = false;
        std::int32_t currentLine = 0;

        for (std::int32_t i = r.start; i < r.end; ++i) {
            const TextChar& ch = chars_[static_cast<std::size_t>(i)];
            // 換行字元的外框是退化的（零寬或落在行首），併進去會拉歪整個 quad。
            if (ch.category() == CharCategory::Control || ch.box.isEmpty()) continue;
            if (hasCurrent && ch.lineIndex != currentLine) {
                result.push_back(QuadPoint::fromRect(current));
                hasCurrent = false;
            }
            current = hasCurrent ? current.united(ch.box) : ch.box;
            currentLine = ch.lineIndex;
            hasCurrent = true;
        }
        if (hasCurrent) result.push_back(QuadPoint::fromRect(current));
        return result;
    }

    [[nodiscard]] RectF boundingBox(TextRange range) const {
        const TextRange r = range.clamped(charCount());
        RectF box{};
        for (std::int32_t i = r.start; i < r.end; ++i) {
            const TextChar& ch = chars_[static_cast<std::size_t>(i)];
            if (ch.box.isEmpty()) continue;
            box = box.united(ch.box);
        }
        return box;
    }

    // 純領域的命中測試，用於已擷取並快取的文字層（拖曳選取時每幀都會問）。
    // 首次點擊的權威判定仍走引擎的 FPDFText_GetCharIndexAtPos，兩者容忍度語意相同。
    [[nodiscard]] std::int32_t charIndexNear(const PointF& p, double tolerance) const noexcept {
        std::int32_t best = -1;
        double bestDistance = 0.0;
        for (const TextChar& ch : chars_) {
            if (ch.category() == CharCategory::Control || ch.box.isEmpty()) continue;
            const RectF hit{ch.box.left - tolerance, ch.box.bottom - tolerance,
                            ch.box.right + tolerance, ch.box.top + tolerance};
            if (!hit.contains(p)) continue;
            const double dx = (ch.box.left + ch.box.right) * 0.5 - p.x;
            const double dy = (ch.box.bottom + ch.box.top) * 0.5 - p.y;
            const double distance = dx * dx + dy * dy;
            if (best < 0 || distance < bestDistance) {
                best = ch.index;
                bestDistance = distance;
            }
        }
        return best;
    }

    // 雙擊選詞（PRD-TXT-004）。不跨行，避免行尾雙擊選到下一行的字。
    [[nodiscard]] TextRange wordRangeAt(std::int32_t index) const noexcept {
        const TextChar* ch = charAt(index);
        if (!ch) return TextRange{};
        const CharCategory category = ch->category();
        if (category == CharCategory::Control) return TextRange{};
        // 表意文字沒有詞界，斷詞需要詞典；此處只取單字，優於猜錯整句。
        if (category == CharCategory::Ideograph || category == CharCategory::Punctuation) {
            return TextRange{index, index + 1};
        }

        const std::int32_t line = ch->lineIndex;
        std::int32_t start = index;
        while (start > 0) {
            const TextChar& prev = chars_[static_cast<std::size_t>(start) - 1];
            if (prev.lineIndex != line || prev.category() != category) break;
            --start;
        }
        std::int32_t end = index + 1;
        while (end < charCount()) {
            const TextChar& next = chars_[static_cast<std::size_t>(end)];
            if (next.lineIndex != line || next.category() != category) break;
            ++end;
        }
        return TextRange{start, end};
    }

    // 三擊選行（PRD-TXT-004）。行尾的控制字元不納入，否則 quad 會多出一截。
    [[nodiscard]] TextRange lineRangeAt(std::int32_t index) const noexcept {
        const TextChar* ch = charAt(index);
        if (!ch) return TextRange{};
        const std::int32_t line = ch->lineIndex;
        std::int32_t start = index;
        while (start > 0 && chars_[static_cast<std::size_t>(start) - 1].lineIndex == line) --start;
        std::int32_t end = index + 1;
        while (end < charCount() && chars_[static_cast<std::size_t>(end)].lineIndex == line) ++end;
        while (end > start && chars_[static_cast<std::size_t>(end) - 1].category() ==
                                  CharCategory::Control) {
            --end;
        }
        return TextRange{start, end};
    }

    [[nodiscard]] std::int32_t lineCount() const noexcept {
        return chars_.empty() ? 0 : chars_.back().lineIndex + 1;
    }

private:
    std::int32_t pageIndex_{0};
    std::vector<TextChar> chars_;
};

}  // namespace alioth::domain
