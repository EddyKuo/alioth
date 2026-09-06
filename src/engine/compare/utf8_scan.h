#pragma once

// UTF-8 掃描的最小工具集。
//
// 只做「往前／往後讀一個碼點」這件事，不做正規化也不做大小寫對映——
// 那些需要 ICU，而本專案的引擎級相依上限是三個元件（CLAUDE.md），加不進來。
//
// 領域層的 domain::appendUtf8 是編碼方向，這裡是解碼方向；兩者互為反向，
// 不重複實作同一件事。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace alioth::engine::compare {

// 從 pos 讀一個碼點，回傳消耗的位元組數；pos 已到結尾時回傳 0。
// 非法序列以「一個位元組、碼點為 U+FFFD」處理：不可信任輸入不得讓掃描停住或倒退。
inline std::size_t decodeUtf8(std::string_view text, std::size_t pos, char32_t& out) noexcept {
    if (pos >= text.size()) {
        out = 0;
        return 0;
    }
    const auto byte = static_cast<unsigned char>(text[pos]);
    std::size_t length = 1;
    char32_t value = byte;
    if (byte < 0x80) {
        out = value;
        return 1;
    } else if ((byte & 0xE0) == 0xC0) {
        length = 2;
        value = byte & 0x1Fu;
    } else if ((byte & 0xF0) == 0xE0) {
        length = 3;
        value = byte & 0x0Fu;
    } else if ((byte & 0xF8) == 0xF0) {
        length = 4;
        value = byte & 0x07u;
    } else {
        out = 0xFFFD;
        return 1;
    }
    if (pos + length > text.size()) {
        out = 0xFFFD;
        return 1;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto cont = static_cast<unsigned char>(text[pos + i]);
        if ((cont & 0xC0) != 0x80) {
            out = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (cont & 0x3Fu);
    }
    out = value;
    return length;
}

// pos 之前的那個碼點。pos 為 0 時回傳 0（視同文字開頭）。
inline char32_t codepointBefore(std::string_view text, std::size_t pos) noexcept {
    if (pos == 0 || pos > text.size()) return 0;
    std::size_t start = pos - 1;
    // 續接位元組最多三個，往回找超過四格就是壞資料，直接當作單一位元組處理。
    std::size_t steps = 0;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80 && steps < 3) {
        --start;
        ++steps;
    }
    char32_t value = 0;
    const std::size_t consumed = decodeUtf8(text, start, value);
    return consumed == pos - start ? value : static_cast<char32_t>(
                                                 static_cast<unsigned char>(text[pos - 1]));
}

// ASCII 範圍的大小寫摺疊。
//
// 只做 ASCII 是刻意的取捨並且必須寫進 UI 說明：完整的 Unicode 大小寫摺疊
// （土耳其語的 i、德語的 ß、希臘語的尾 sigma）需要 ICU。做半套 Unicode 比只做 ASCII
// 更糟——使用者會以為它全都對。
[[nodiscard]] inline std::string foldAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : c);
    }
    return out;
}

}  // namespace alioth::engine::compare
