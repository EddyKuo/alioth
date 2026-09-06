#pragma once

// 文字子系統內部的編碼轉換。
//
// PDFium 對外一律是 UTF-16（FPDF_WIDESTRING 是 unsigned short*），
// 領域層一律是 UTF-8。轉換集中在這裡，避免各處各寫一份而在代理對上分歧。
// 不用 Qt 的 QString：引擎轉接層不得依賴 Qt。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/text_layer.h"

namespace alioth::engine::text {

// UTF-8 → UTF-16，附帶結尾 NUL，可直接當 FPDF_WIDESTRING 用。
[[nodiscard]] inline std::vector<unsigned short> toUtf16(const std::string& utf8) {
    std::vector<unsigned short> out;
    out.reserve(utf8.size() + 1);
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (byte < 0x80) {
            cp = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            cp = byte & 0x1Fu;
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            cp = byte & 0x0Fu;
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            cp = byte & 0x07u;
            extra = 3;
        } else {
            // 非法起始位元組：跳過而非中止，搜尋字串來自使用者輸入，不該讓它炸掉。
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) break;
        for (std::size_t k = 1; k <= extra; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        i += extra + 1;

        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<unsigned short>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<unsigned short>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<unsigned short>(cp));
        }
    }
    out.push_back(0);
    return out;
}

// UTF-16 → UTF-8。代理對在此合併，落單的代理項直接丟棄。
[[nodiscard]] inline std::string toUtf8(const unsigned short* data, std::size_t length) {
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        char32_t cp = data[i];
        if (cp == 0) continue;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= length) continue;
            const char32_t low = data[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) continue;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            ++i;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;
        }
        domain::appendUtf8(out, cp);
    }
    return out;
}

}  // namespace alioth::engine::text
