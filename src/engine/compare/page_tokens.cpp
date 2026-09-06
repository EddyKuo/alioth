#include "engine/compare/page_tokens.h"

#include "engine/compare/utf8_scan.h"

namespace alioth::engine::compare {
namespace {

using domain::CharCategory;
using domain::TextRange;

// 兩個進入點（文字層與純文字）共用同一套斷詞規則。
// 規則寫兩遍的話，端到端測試與單元測試會在不同的規則上通過，等於沒測到。
class TokenBuilder {
public:
    TokenBuilder(TokenTable& table, bool ignoreCase, PageTokens& out)
        : table_(table), ignoreCase_(ignoreCase), out_(out) {}

    void feed(char32_t c, std::int32_t begin, std::int32_t end) {
        const CharCategory category = domain::categorize(c);
        switch (category) {
            case CharCategory::Control:
            case CharCategory::Whitespace:
                flush();
                return;
            case CharCategory::Word:
                if (!open_) {
                    open_ = true;
                    pending_.charRange.start = begin;
                    pending_.text.clear();
                }
                pending_.charRange.end = end;
                domain::appendUtf8(pending_.text, c);
                return;
            case CharCategory::Ideograph:
            case CharCategory::Punctuation:
                // 表意文字沒有詞界，斷詞需要詞典；標點單獨成 token 讓「加了一個逗號」
                // 不會把整句話標成變更。
                flush();
                emit(c, begin, end);
                return;
        }
    }

    void flush() {
        if (!open_) return;
        open_ = false;
        push(std::move(pending_.text), pending_.charRange);
        pending_ = Token{};
    }

    void finish() {
        flush();
        // FNV-1a：只用來當「兩頁 token 序列是否完全相同」的快速前置判斷，
        // 相等時仍會逐 token 驗證，因此不需要密碼學強度。
        std::uint64_t hash = 1469598103934665603ull;
        for (const Token& t : out_.tokens) {
            std::uint64_t id = t.id;
            for (int i = 0; i < 4; ++i) {
                hash ^= static_cast<std::uint64_t>(id & 0xFFu);
                hash *= 1099511628211ull;
                id >>= 8;
            }
        }
        out_.contentHash = hash;
    }

private:
    void emit(char32_t c, std::int32_t begin, std::int32_t end) {
        std::string text;
        domain::appendUtf8(text, c);
        push(std::move(text), TextRange{begin, end});
    }

    void push(std::string text, TextRange range) {
        Token token{};
        token.charRange = range;
        token.id = table_.intern(ignoreCase_ ? foldAscii(text) : text);
        token.text = std::move(text);
        out_.tokens.push_back(std::move(token));
    }

    TokenTable& table_;
    bool ignoreCase_{false};
    PageTokens& out_;
    Token pending_{};
    bool open_{false};
};

}  // namespace

PageTokens tokenizePage(const domain::PageTextLayer& layer, TokenTable& table, bool ignoreCase) {
    PageTokens out{};
    out.pageIndex = layer.pageIndex();
    TokenBuilder builder(table, ignoreCase, out);

    const std::vector<domain::TextChar>& chars = layer.chars();
    const std::int32_t count = static_cast<std::int32_t>(chars.size());
    for (std::int32_t i = 0; i < count; ++i) {
        const std::int32_t begin = i;
        char32_t c = chars[static_cast<std::size_t>(i)].unicode;
        // 擷取時保留 PDFium 的每個索引，因此代理對是兩個相鄰的字元；
        // 合併回單一碼點才能正確分類（CJK 擴充 B 區都在補充平面）。
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < count) {
            const char32_t low = chars[static_cast<std::size_t>(i) + 1].unicode;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (c == 0) continue;
        builder.feed(c, begin, i + 1);
    }
    builder.finish();
    return out;
}

PageTokens tokenizeText(std::int32_t pageIndex, std::string_view utf8, TokenTable& table,
                        bool ignoreCase) {
    PageTokens out{};
    out.pageIndex = pageIndex;
    TokenBuilder builder(table, ignoreCase, out);

    std::size_t pos = 0;
    std::int32_t index = 0;
    while (pos < utf8.size()) {
        char32_t c = 0;
        const std::size_t consumed = decodeUtf8(utf8, pos, c);
        if (consumed == 0) break;
        pos += consumed;
        builder.feed(c, index, index + 1);
        ++index;
    }
    builder.finish();
    return out;
}

}  // namespace alioth::engine::compare
