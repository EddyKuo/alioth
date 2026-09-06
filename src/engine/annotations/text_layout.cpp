#include "engine/annotations/text_layout.h"

#include <cstdio>

#include "engine/fonts/cjk_font_library.h"

#include <algorithm>
#include <cmath>

namespace alioth::engine::annotations {

namespace {

// 與 engine/formbuild/field_appearance.cpp::estimateHelveticaWidth 相同的分佈
// （Helvetica AFM 概略字寬，單位 1/1000 em）：數字與多數小寫約 0.556 em、
// 大寫約 0.667 em、窄字元約 0.28 em。這份數字本身沒有問題，會重複出現在
// engine/formbuild 是因為那個 target 對「非法輸入」的處置策略與這裡不同
// （靜默丟棄 vs 明確失敗），見 text_layout.h 頂端註解。
[[nodiscard]] double glyphWidthUnits(unsigned char c) noexcept {
    if (c == ' ') return 278.0;
    if (c == 'i' || c == 'l' || c == 'j' || c == '.' || c == ',' || c == '\'' || c == ':' ||
        c == ';' || c == '|' || c == '!') {
        return 250.0;
    }
    if (c >= 'A' && c <= 'Z') return 667.0;
    if (c == 'm' || c == 'w' || c == 'M' || c == 'W') return 833.0;
    return 556.0;
}

// 逐字元檢查是否落在可列印 ASCII（含空白）。控制字元只允許 \n，
// \r 在正規化階段就已經被去掉，走到這裡不該再出現。
[[nodiscard]] bool isRenderableAscii(unsigned char c) noexcept {
    return (c >= 0x20 && c < 0x7F) || c == '\n';
}

// 把 \r\n 與孤立的 \r 一律正規化成 \n，避免同一份文字在不同來源系統下
// 換行判斷不一致。
[[nodiscard]] std::string normalizeNewlines(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            continue;
        }
        out += c;
    }
    return out;
}

[[nodiscard]] std::vector<std::string> splitByNewline(const std::string& text) {
    std::vector<std::string> paragraphs;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            paragraphs.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    paragraphs.push_back(current);
    return paragraphs;
}

// 把一個段落貪婪地依單字換行到 maxWidth 以內。單一單字本身超寬時不再細分
// （沒有斷字規則），讓它獨佔一行溢出——這與 Acrobat 對超長無空白字串的處理
// 一致，寧可讓使用者看到溢出也不要在字中間硬切。
// UTF-8 → 碼點。非法位元組跳過而不是換成替代字元：替代字元會變成一個
// 使用者沒輸入過的字，而跳過至少不會無中生有。
std::u32string decodeUtf8(const std::string& text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        char32_t value = 0;
        if (byte < 0x80) {
            value = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            value = byte & 0x1F;
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            value = byte & 0x0F;
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            value = byte & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (i + extra >= text.size()) break;
        bool ok = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            value = (value << 6) | (continuation & 0x3F);
        }
        if (ok) out.push_back(value);
        i += extra + 1;
    }
    return out;
}

void appendUtf8(std::string& out, char32_t c) {
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

// CJK 可以在任兩個字之間換行，拉丁文不行——那是兩種語言的排版規則差異，
// 不是實作偷懶。把中文當成一個「詞」會讓一整段中文永遠是一行，直接衝出框外。
//
// 這裡只做最基本的判定，不處理禁則（行首不得為「，。」之類）——
// 那需要完整的斷行規則表，屬於排版層，而 PRD §2.1 已排除排版層。
[[nodiscard]] bool breaksAnywhere(char32_t c) noexcept {
    return (c >= 0x2E80 && c <= 0x9FFF) ||    // CJK 部首至統一表意文字
           (c >= 0xAC00 && c <= 0xD7AF) ||    // 諺文
           (c >= 0xF900 && c <= 0xFAFF) ||    // 相容表意文字
           (c >= 0xFF00 && c <= 0xFF60) ||    // 全形標點與字母
           (c >= 0x3000 && c <= 0x303F) ||    // CJK 標點
           (c >= 0x20000 && c <= 0x2FA1F);    // 擴充區
}

[[nodiscard]] std::vector<std::string> wrapParagraph(const std::string& paragraph, double fontSize,
                                                      double maxWidth) {
    if (paragraph.empty()) return {std::string{}};
    if (!(maxWidth > 0.0)) return {paragraph};

    std::vector<std::string> lines;
    std::string line;
    std::string word;

    auto flushWord = [&]() {
        if (word.empty()) return;
        if (line.empty()) {
            line = word;
        } else {
            // 接續時要不要加空白，取決於兩邊是不是 CJK。中文字之間插空白
            // 會讓整段中文變成「中 文 字 距 很 寬」，那是很明顯的錯。
            const std::u32string lineChars = decodeUtf8(line);
            const std::u32string wordChars = decodeUtf8(word);
            const bool joinTightly =
                (!lineChars.empty() && breaksAnywhere(lineChars.back())) ||
                (!wordChars.empty() && breaksAnywhere(wordChars.front()));
            const std::string candidate = joinTightly ? line + word : line + " " + word;
            if (estimateTextWidth(candidate, fontSize) <= maxWidth) {
                line = candidate;
            } else {
                lines.push_back(line);
                line = word;
            }
        }
        word.clear();
    };

    // 逐碼點而不是逐位元組：UTF-8 的一個中文字是三個位元組，
    // 逐位元組累積會在字的中間斷開，產生無效的 UTF-8。
    for (const char32_t codepoint : decodeUtf8(paragraph)) {
        if (codepoint == U' ') {
            flushWord();
            continue;
        }
        if (breaksAnywhere(codepoint)) {
            // CJK 每個字自成一個可斷點。先把前面累積的拉丁詞送出去，
            // 再把這個字當成獨立的「詞」處理。
            flushWord();
            appendUtf8(word, codepoint);
            flushWord();
            continue;
        }
        appendUtf8(word, codepoint);
    }
    flushWord();
    lines.push_back(line);
    return lines;
}

}  // namespace

double estimateTextWidth(const std::string& asciiLine, double fontSize) {
    double units = 0.0;
    for (const char32_t codepoint : decodeUtf8(asciiLine)) {
        if (codepoint < 128) {
            units += glyphWidthUnits(static_cast<unsigned char>(codepoint));
            continue;
        }
        // 非 ASCII 走內嵌字型的實際字寬。**量測與內嵌必須是同一份字型**：
        // 用別的字型量出來的寬度會讓換行位置與畫出來的不一致，文字不是超出框
        // 就是框底下多一塊空白，而且不會有任何錯誤訊息。
        const std::uint16_t advance = fonts::CjkFontLibrary::instance().advanceFor(codepoint);
        // 字型裡沒有這個字時退回一個全形寬度。這條路徑只會在 layoutText 已經
        // 確認過所有字都畫得出來之後才走到，所以這個退路實際上不會被用到；
        // 留著是為了讓「量測」不會因為缺字而回傳一個荒謬的小寬度。
        units += advance != 0 ? advance : 1000.0;
    }
    return units / 1000.0 * fontSize;
}

TextLayoutResult layoutText(const std::string& utf8Text, const TextFitOptions& options) {
    TextLayoutResult result{};
    if (!(options.fontSize > 0.0)) {
        result.diagnostic = "字級必須大於零";
        return result;
    }

    const std::string normalized = normalizeNewlines(utf8Text);

    // 非 ASCII 走內嵌 CJK 字型（ADR-007）。逐字檢查而不是「有沒有非 ASCII」：
    // 缺字必須指出是**哪一個字**，否則使用者面對一段長文字完全不知道要改哪裡。
    for (const char32_t codepoint : decodeUtf8(normalized)) {
        if (codepoint < 128) {
            if (isRenderableAscii(static_cast<unsigned char>(codepoint))) continue;
            result.diagnostic = "文字含無法畫出的控制字元";
            return result;
        }
        auto& library = fonts::CjkFontLibrary::instance();
        if (!library.available()) {
            result.diagnostic = "沒有可用的 CJK 字型，無法畫出非 ASCII 文字。" +
                                library.diagnostic();
            return result;
        }
        if (library.advanceFor(codepoint) == 0) {
            // 靜默丟字會讓表單欄位的 /V 有值而畫面空白，使用者不會發現。
            // 指出是哪個碼點缺字，使用者才有辦法換一個字或換字型。
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "U+%04X",
                          static_cast<unsigned>(codepoint));
            result.diagnostic = std::string("字型缺少這個字：") + buffer;
            return result;
        }
    }

    for (const std::string& paragraph : splitByNewline(normalized)) {
        for (std::string& line : wrapParagraph(paragraph, options.fontSize, options.maxWidth)) {
            result.contentWidth = std::max(result.contentWidth, estimateTextWidth(line, options.fontSize));
            result.lines.push_back(std::move(line));
        }
    }

    result.lineHeight = options.fontSize * options.lineSpacingRatio;
    result.contentHeight = result.lineHeight * static_cast<double>(result.lines.size());
    result.ok = true;
    return result;
}

FitBoxResult fitBoxByTextContent(const domain::RectF& box, const std::string& utf8Text,
                                 const FitBoxOptions& options) {
    FitBoxResult result{};
    const domain::RectF normalizedBox = box.normalized();
    const double padding = std::max(0.0, options.paddingPt);

    // 寬度已由使用者拖出：沿用寬度，只依內容重算高度。
    // 寬度未定（<= 0）：退化成單行的自然寬度，讓框「剛好包住文字」。
    const bool widthGiven = normalizedBox.width() > 0.0;
    TextFitOptions fit{};
    fit.fontSize = options.fontSize;
    fit.maxWidth = widthGiven ? std::max(0.0, normalizedBox.width() - padding * 2.0) : 0.0;

    TextLayoutResult layout = layoutText(utf8Text, fit);
    if (!layout.ok) {
        result.diagnostic = layout.diagnostic;
        return result;
    }
    result.fontSize = fit.fontSize;

    // 固定框模式：高度不動，字級往下找到塞得進去的那一級。
    //
    // 每一級都要重新換行，不能只按比例換算高度：字變小之後同一段文字可能
    // 少斷一行，行數本身就是字級的函數。按比例算會系統性高估所需高度，
    // 於是縮得比實際需要更小。
    if (options.maxHeight > 0.0) {
        const double available = std::max(0.0, options.maxHeight - padding * 2.0);
        const double step = options.fontSizeStepPt > 0.0 ? options.fontSizeStepPt : 0.5;
        const double floorSize = std::max(0.1, options.minFontSize);
        while (layout.contentHeight > available && fit.fontSize > floorSize) {
            fit.fontSize = std::max(floorSize, fit.fontSize - step);
            // 寬度上限不變（框的寬度沒有跟著縮），只有字級變。
            TextLayoutResult retry = layoutText(utf8Text, fit);
            if (!retry.ok) {
                result.diagnostic = retry.diagnostic;
                return result;
            }
            layout = std::move(retry);
        }
        result.fontSize = fit.fontSize;
        result.overflows = layout.contentHeight > available;
    }

    result.layout = layout;

    const double width = widthGiven ? normalizedBox.width() : layout.contentWidth + padding * 2.0;
    const double height = options.maxHeight > 0.0
                              ? options.maxHeight
                              : std::max(options.minHeight, layout.contentHeight + padding * 2.0);

    // 錨點是左上角：top 不變，width 依上述規則決定，height 由內容或固定框決定。
    result.rect = domain::RectF{normalizedBox.left, normalizedBox.top - height,
                                normalizedBox.left + width, normalizedBox.top};
    result.ok = true;
    return result;
}

}  // namespace alioth::engine::annotations
