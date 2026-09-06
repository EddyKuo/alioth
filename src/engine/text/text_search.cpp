#include "engine/text/text_search.h"

#include <fpdf_text.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#include "engine/text/text_encoding.h"

namespace alioth::engine::text {
namespace {

// 取出字元區間的文字。FPDFText_GetText 的回傳值含結尾 NUL，緩衝區要多留一格。
std::string textSlice(FPDF_TEXTPAGE textPage, std::int32_t start, std::int32_t count) {
    if (count <= 0) return {};
    std::vector<unsigned short> buffer(static_cast<std::size_t>(count) + 1, 0);
    const int written = FPDFText_GetText(textPage, start, count, buffer.data());
    if (written <= 1) return {};
    return toUtf8(buffer.data(), static_cast<std::size_t>(written - 1));
}

// 結果列表是單行的；把換行與定位字元換成空白，否則上下文會把列表撐開或截斷。
std::string flattenForDisplay(std::string text) {
    for (char& c : text) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    return text;
}

}  // namespace

std::vector<domain::SearchResult> searchPage(const TextPage& page, const std::string& queryUtf8,
                                             const SearchOptions& options,
                                             const CancellationToken& token) {
    std::vector<domain::SearchResult> results;
    if (!page.valid() || queryUtf8.empty()) return results;

    auto* textPage = static_cast<FPDF_TEXTPAGE>(page.handle());
    const std::vector<unsigned short> query = toUtf16(queryUtf8);
    if (query.size() <= 1) return results;  // 只剩結尾 NUL：輸入全是非法位元組

    unsigned long flags = 0;
    if (options.matchCase) flags |= FPDF_MATCHCASE;
    if (options.matchWholeWord) flags |= FPDF_MATCHWHOLEWORD;

    FPDF_SCHHANDLE handle = FPDFText_FindStart(
        textPage, reinterpret_cast<FPDF_WIDESTRING>(query.data()), flags, 0);
    if (!handle) return results;

    const std::int32_t total = charCount(page);
    const std::int32_t radius = std::max(0, options.contextRadius);

    while (FPDFText_FindNext(handle)) {
        const std::int32_t index = FPDFText_GetSchResultIndex(handle);
        const std::int32_t count = FPDFText_GetSchCount(handle);
        if (index < 0 || count <= 0) break;

        const std::int32_t contextStart = std::max(0, index - radius);
        const std::int32_t contextEnd = std::min(total, index + count + radius);

        domain::SearchResult result;
        result.pageIndex = page.pageIndex();
        result.range = domain::TextRange{index, index + count};

        const std::string prefix =
            flattenForDisplay(textSlice(textPage, contextStart, index - contextStart));
        const std::string match = flattenForDisplay(textSlice(textPage, index, count));
        const std::string suffix =
            flattenForDisplay(textSlice(textPage, index + count, contextEnd - index - count));

        result.context = prefix + match + suffix;
        result.matchOffset = static_cast<std::int32_t>(prefix.size());
        result.matchLength = static_cast<std::int32_t>(match.size());
        results.push_back(std::move(result));

        if (static_cast<std::int32_t>(results.size()) >= options.maxMatchesPerPage) break;
        if (token.valid() && token.isCancelled()) break;
    }

    FPDFText_FindClose(handle);
    return results;
}

struct SearchSession::State {
    TextExtractor* extractor{nullptr};
    std::string query;
    SearchOptions options{};
    std::vector<std::int32_t> order;
    std::size_t cursor{0};
    PageCallback onPage;
    FinishedCallback onFinished;
    CancellationSource cancellation;
    SearchSummary summary{};
    std::atomic<bool> running{false};
};

// 刻意一次只排一頁：整份文件一次全排進佇列的話，取消要等佇列跑完才生效，
// 而且會擋住同一條執行緒上的選取查詢。
void SearchSession::scheduleNext(const std::shared_ptr<State>& state) {
    const CancellationToken token = state->cancellation.token();
    if (token.isCancelled() || state->cursor >= state->order.size()) {
        state->summary.cancelled = token.isCancelled();
        state->running.store(false, std::memory_order_release);
        if (state->onFinished) state->onFinished(state->summary);
        return;
    }

    const std::int32_t pageIndex = state->order[state->cursor++];
    state->extractor->withTextPage(pageIndex, [state, pageIndex, token](const TextPage* page) {
        if (!token.isCancelled() && page && page->valid()) {
            auto hits = searchPage(*page, state->query, state->options, token);
            ++state->summary.pagesScanned;
            state->summary.totalMatches += static_cast<std::int32_t>(hits.size());
            if (!hits.empty() && state->onPage) state->onPage(pageIndex, std::move(hits));
        }
        scheduleNext(state);
    });
}

SearchSession::SearchSession(TextExtractor& extractor) : state_(std::make_shared<State>()) {
    state_->extractor = &extractor;
}

SearchSession::~SearchSession() {
    // 工作以 shared_ptr 持有狀態，本物件消失後仍可能有一頁在飛。
    // 取消它，讓回呼不會再往下排新頁。
    if (state_) state_->cancellation.cancelAll();
}

void SearchSession::start(std::string queryUtf8, SearchOptions options, std::int32_t startPage,
                          PageCallback onPage, FinishedCallback onFinished) {
    state_->query = std::move(queryUtf8);
    state_->options = options;
    state_->onPage = std::move(onPage);
    state_->onFinished = std::move(onFinished);
    state_->summary = {};
    state_->cursor = 0;
    state_->cancellation.reset();

    const std::int32_t pageCount = state_->extractor->pageCount();
    state_->order.clear();
    if (pageCount <= 0 || state_->query.empty()) {
        state_->running.store(false, std::memory_order_release);
        if (state_->onFinished) state_->onFinished(state_->summary);
        return;
    }

    // 從目前頁往後掃，掃到底再從頭補齊，這樣「找下一個」不會漏掉起始頁之前的命中。
    state_->order.reserve(static_cast<std::size_t>(pageCount));
    const std::int32_t first = (startPage < 0 || startPage >= pageCount) ? 0 : startPage;
    for (std::int32_t i = first; i < pageCount; ++i) state_->order.push_back(i);
    for (std::int32_t i = 0; i < first; ++i) state_->order.push_back(i);

    state_->running.store(true, std::memory_order_release);
    scheduleNext(state_);
}

void SearchSession::cancel() { state_->cancellation.cancelAll(); }

bool SearchSession::isRunning() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

}  // namespace alioth::engine::text
