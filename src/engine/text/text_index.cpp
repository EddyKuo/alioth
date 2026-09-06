#include "engine/text/text_index.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace alioth::engine::text {
namespace {

// 大小寫摺疊只做 ASCII 與拉丁文擴充區的簡單對應。
//
// 刻意不引入完整的 Unicode 大小寫規則（那需要 ICU，違反「僅三個引擎級元件」）。
// 這與 PDFium 的 FPDF_MATCHCASE 行為一致——它做的也是簡單摺疊，
// 所以索引搜尋與逐頁搜尋在同一份文件上會得到同一組結果，兩者可以互相驗證。
[[nodiscard]] char32_t foldCase(char32_t c) noexcept {
    if (c >= U'A' && c <= U'Z') return c + 32;
    // 拉丁文-1 補充的大寫區（不含 0xD7 乘號）。
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 32;
    return c;
}

[[nodiscard]] std::u32string foldCase(const std::u32string& text) {
    std::u32string out;
    out.reserve(text.size());
    for (const char32_t c : text) out.push_back(foldCase(c));
    return out;
}

// UTF-8 → char32_t 序列。非法位元組直接跳過而不是換成替代字元：
// 查詢字串裡的壞位元組是輸入錯誤，把它變成 U+FFFD 會讓它去比對文件裡真的有
// U+FFFD 的地方，那是無中生有的命中。
[[nodiscard]] std::u32string decodeUtf8(const std::string& text) {
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

// 索引裡的代理對是兩個相鄰字元（維持 PDFium 的索引），而查詢解碼出來的是
// 單一碼點。比對前把查詢也拆成代理對，兩邊才對得起來。
[[nodiscard]] std::u32string toSurrogateForm(const std::u32string& text) {
    std::u32string out;
    out.reserve(text.size() + 4);
    for (const char32_t c : text) {
        if (c >= 0x10000) {
            const char32_t v = c - 0x10000;
            out.push_back(0xD800 + (v >> 10));
            out.push_back(0xDC00 + (v & 0x3FF));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] bool isWordChar(char32_t c) noexcept {
    const domain::CharCategory category = domain::categorize(c);
    return category == domain::CharCategory::Word ||
           category == domain::CharCategory::Ideograph;
}

// 整字比對：命中的前後不得再是文字字元。與 FPDF_MATCHWHOLEWORD 同義。
[[nodiscard]] bool isWholeWord(const std::u32string& text, std::size_t start, std::size_t length) {
    if (start > 0 && isWordChar(text[start - 1])) return false;
    const std::size_t after = start + length;
    if (after < text.size() && isWordChar(text[after])) return false;
    return true;
}

// 結果列表是單行的；換行與定位字元會把列表撐開或截斷。
// 與 text_search.cpp 的 flattenForDisplay 同一個理由與同一個結果。
void appendFlattened(std::string& out, char32_t c) {
    if (c == U'\r' || c == U'\n' || c == U'\t') {
        out.push_back(' ');
        return;
    }
    domain::appendUtf8(out, c);
}

// 把 [start, end) 轉成 UTF-8，代理對合併回單一碼點——與 PageTextLayer::text 一致。
[[nodiscard]] std::string sliceUtf8(const std::u32string& text, std::size_t start,
                                    std::size_t end) {
    std::string out;
    out.reserve((end - start) * 2);
    for (std::size_t i = start; i < end && i < text.size(); ++i) {
        char32_t c = text[i];
        if (c == 0) continue;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < end && i + 1 < text.size()) {
            const char32_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        appendFlattened(out, c);
    }
    return out;
}

}  // namespace

void TextIndex::setPage(std::int32_t pageIndex, std::u32string text) {
    const std::lock_guard lock(mutex_);
    pages_[pageIndex] = std::move(text);
}

void TextIndex::setPageFromLayer(const domain::PageTextLayer& layer) {
    std::u32string text;
    text.reserve(layer.chars().size());
    for (const domain::TextChar& c : layer.chars()) text.push_back(c.unicode);
    setPage(layer.pageIndex(), std::move(text));
}

bool TextIndex::hasPage(std::int32_t pageIndex) const {
    const std::lock_guard lock(mutex_);
    return pages_.find(pageIndex) != pages_.end();
}

std::int32_t TextIndex::indexedPageCount() const {
    const std::lock_guard lock(mutex_);
    return static_cast<std::int32_t>(pages_.size());
}

void TextIndex::clear() {
    const std::lock_guard lock(mutex_);
    pages_.clear();
}

std::vector<domain::SearchResult> TextIndex::search(const std::string& queryUtf8,
                                                    const SearchOptions& options,
                                                    std::int32_t startPage,
                                                    const CancellationToken& token) const {
    std::vector<domain::SearchResult> results;
    // 整段搜尋都持有鎖。掃一次是毫秒級（500 頁量到 4.2 毫秒），
    // 而中途放掉鎖會讓「已索引頁數」與實際掃到的頁不一致，
    // 那正是「搜尋結果偶爾少一頁」這種查不到原因的缺陷。
    const std::lock_guard lock(mutex_);
    if (queryUtf8.empty() || pages_.empty()) return results;

    const std::u32string decoded = toSurrogateForm(decodeUtf8(queryUtf8));
    if (decoded.empty()) return results;
    const std::u32string needle = options.matchCase ? decoded : foldCase(decoded);

    // 掃描順序：startPage 之後先掃，再繞回開頭。與 SearchSession 一致，
    // 「從目前頁開始找」因此不會因為換了實作而變成從第一頁開始。
    std::vector<std::int32_t> order;
    order.reserve(pages_.size());
    for (const auto& entry : pages_) order.push_back(entry.first);
    std::sort(order.begin(), order.end());
    const auto pivot = std::lower_bound(order.begin(), order.end(), startPage);
    std::rotate(order.begin(), pivot, order.end());

    const std::int32_t radius = std::max(0, options.contextRadius);

    for (const std::int32_t pageIndex : order) {
        if (token.valid() && token.isCancelled()) break;

        const std::u32string& raw = pages_.at(pageIndex);
        if (raw.size() < needle.size()) continue;
        const std::u32string haystack = options.matchCase ? raw : foldCase(raw);

        std::int32_t matchesOnPage = 0;
        std::size_t from = 0;
        while (true) {
            const std::size_t hit = haystack.find(needle, from);
            if (hit == std::u32string::npos) break;

            // 前進一整個命中長度，**不找重疊的命中**：在 "aaaa" 裡找 "aa" 是兩個，
            // 不是三個。這是為了與 PDFium 的 FPDFText_FindNext 一致——
            // 索引是來取代逐頁搜尋的，兩者只要有一處不同，同一份文件換個入口
            // 就會看到不同的命中數，而那不會有任何錯誤訊息。
            // （這條規則是被 test_text_index 的交叉比對抓出來的，不是推導出來的。）
            from = hit + needle.size();

            if (options.matchWholeWord && !isWholeWord(haystack, hit, needle.size())) continue;

            const std::size_t contextStart =
                hit > static_cast<std::size_t>(radius) ? hit - static_cast<std::size_t>(radius) : 0;
            const std::size_t contextEnd =
                std::min(raw.size(), hit + needle.size() + static_cast<std::size_t>(radius));

            domain::SearchResult result;
            result.pageIndex = pageIndex;
            result.range = domain::TextRange{static_cast<std::int32_t>(hit),
                                             static_cast<std::int32_t>(hit + needle.size())};

            const std::string prefix = sliceUtf8(raw, contextStart, hit);
            const std::string match = sliceUtf8(raw, hit, hit + needle.size());
            result.context = prefix + match + sliceUtf8(raw, hit + needle.size(), contextEnd);
            result.matchOffset = static_cast<std::int32_t>(prefix.size());
            result.matchLength = static_cast<std::int32_t>(match.size());
            results.push_back(std::move(result));

            if (++matchesOnPage >= options.maxMatchesPerPage) break;
            if (token.valid() && token.isCancelled()) break;
        }
    }
    return results;
}

struct TextIndexBuilder::State {
    TextExtractor* extractor{nullptr};
    TextIndex* index{nullptr};
    std::int32_t pageCount{0};
    std::int32_t nextPage{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{false};
    ProgressCallback onProgress;

    // 一次排一頁，做完再排下一頁。整份一次排進佇列的話，取消要等整份做完才生效，
    // 而大型文件的「整份」正是使用者按取消的原因。
    static void scheduleNext(const std::shared_ptr<State>& state) {
        if (state->cancelled.load() || state->nextPage >= state->pageCount) {
            state->running.store(false);
            if (state->onProgress) {
                state->onProgress(IndexProgress{state->nextPage, state->pageCount,
                                                state->cancelled.load()});
            }
            return;
        }

        const std::int32_t page = state->nextPage;
        state->extractor->withTextPage(page, [state, page](const TextPage* textPage) {
            if (textPage != nullptr) {
                state->index->setPageFromLayer(textPage->layer());
            } else {
                // 頁面載不進來時存一個空頁而不是跳過：跳過會讓 indexedPageCount()
                // 永遠到不了 pageCount，呼叫端便會一直以為索引還沒建完。
                state->index->setPage(page, std::u32string{});
            }
            state->nextPage = page + 1;
            if (state->onProgress) {
                state->onProgress(
                    IndexProgress{state->nextPage, state->pageCount, state->cancelled.load()});
            }
            scheduleNext(state);
        });
    }
};

TextIndexBuilder::TextIndexBuilder(TextExtractor& extractor)
    : state_(std::make_shared<State>()) {
    state_->extractor = &extractor;
}

TextIndexBuilder::~TextIndexBuilder() { cancel(); }

void TextIndexBuilder::start(TextIndex& index, std::int32_t pageCount,
                             ProgressCallback onProgress) {
    TextExtractor* const extractor = state_ ? state_->extractor : nullptr;
    cancel();
    // 換一份狀態而不是重設舊的：舊狀態可能還被一個已經投遞出去、正要執行的
    // 工作持有，重設它會讓那個工作寫進新一輪的計數。
    state_ = std::make_shared<State>();
    state_->extractor = extractor;
    state_->index = &index;
    state_->pageCount = pageCount;
    state_->onProgress = std::move(onProgress);
    state_->running.store(pageCount > 0);
    State::scheduleNext(state_);
}

void TextIndexBuilder::cancel() {
    if (state_) state_->cancelled.store(true);
}

bool TextIndexBuilder::isRunning() const { return state_ && state_->running.load(); }

}  // namespace alioth::engine::text
