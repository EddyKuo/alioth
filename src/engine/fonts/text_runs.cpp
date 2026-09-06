#include "engine/fonts/text_runs.h"

#include <cstdio>

#include "engine/fonts/cjk_font_library.h"

namespace alioth::engine::fonts {

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

std::vector<TextRun> splitTextRuns(const std::string& utf8Line) {
    std::vector<TextRun> runs;
    for (const char32_t codepoint : decodeUtf8(utf8Line)) {
        const bool cjk = needsCjkFont(codepoint);
        if (runs.empty() || runs.back().cjk != cjk) runs.push_back(TextRun{cjk, {}, {}});
        TextRun& run = runs.back();

        if (!cjk) {
            run.bytes.push_back(static_cast<char>(codepoint));
            continue;
        }

        const std::uint16_t glyph = CjkFontLibrary::instance().glyphFor(codepoint);
        run.codepoints.insert(codepoint);

        // 兩位元組 big-endian，並跳脫字串字面值裡有特殊意義的位元組。
        // 少了跳脫，一個剛好是 '(' 的位元組會讓字串提前結束，其後所有物件
        // 都解析錯位——那是整份檔案壞掉，不只是一個字錯。
        for (const int shift : {8, 0}) {
            const auto b = static_cast<unsigned char>((glyph >> shift) & 0xFF);
            if (b == 0x28 || b == 0x29 || b == 0x5C) {  // ( ) 反斜線
                run.bytes.push_back(static_cast<char>(0x5C));
                run.bytes.push_back(static_cast<char>(b));
            } else if (b < 32 || b > 126) {
                // 非可列印位元組寫成三位八進位。寫成原始位元組多半也合法，
                // 但 \r 會被某些解析器正規化成 \n，那會讓 GID 直接變成別的字。
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "%c%03o", 0x5C, b);
                run.bytes += buffer;
            } else {
                run.bytes.push_back(static_cast<char>(b));
            }
        }
    }
    return runs;
}

}  // namespace alioth::engine::fonts
