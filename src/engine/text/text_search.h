#pragma once

// 全文搜尋（WBS 2.9，PRD-SRCH-001）。
//
// 逐頁增量是硬性設計而非優化：500 頁文件在 2 秒內要出結果，介面就不能是
// 「掃完整份再回傳一個陣列」——那樣使用者在最後一頁掃完前看不到任何東西，
// 而且取消要等整份掃完才生效。這裡每掃一頁就回報一次，並在頁與頁之間檢查取消。
//
// 所有搜尋都在 TextExtractor 的文字執行緒上進行，與渲染執行緒各自持有獨立的
// 文件把手，因此搜尋不會卡住捲動（CLAUDE.md 硬性限制 1）。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/text_layer.h"
#include "engine/cancellation.h"
#include "engine/text/text_extractor.h"

namespace alioth::engine::text {

struct SearchOptions {
    bool matchCase{false};       // FPDF_MATCHCASE
    bool matchWholeWord{false};  // FPDF_MATCHWHOLEWORD

    // 結果列表要顯示上下文（PRD-SRCH-001），這是命中前後各取幾個字元。
    std::int32_t contextRadius{32};

    // 單頁命中上限。惡意或病態文件可能讓單頁命中數以萬計，
    // 無上限會讓一次搜尋把記憶體吃光（PDF 視為不可信任輸入）。
    std::int32_t maxMatchesPerPage{5000};
};

// 一整次搜尋的結束摘要。cancelled 為真時 pagesScanned 只算實際掃過的頁。
struct SearchSummary {
    std::int32_t pagesScanned{0};
    std::int32_t totalMatches{0};
    bool cancelled{false};
};

// 單頁搜尋。須在文字執行緒上呼叫（例如 TextExtractor::withTextPage 的回呼裡）。
// query 為空字串時回傳空結果，不視為錯誤。
[[nodiscard]] std::vector<domain::SearchResult> searchPage(const TextPage& page,
                                                           const std::string& queryUtf8,
                                                           const SearchOptions& options,
                                                           const CancellationToken& token = {});

// 跨頁搜尋工作階段。每次只排一頁的工作，掃完該頁才排下一頁。
//
// 起始頁之後會繞回文件開頭，讓「從目前頁開始找」與「找下一個」共用同一條路徑。
class SearchSession {
public:
    using PageCallback =
        std::function<void(std::int32_t pageIndex, std::vector<domain::SearchResult>)>;
    using FinishedCallback = std::function<void(SearchSummary)>;

    explicit SearchSession(TextExtractor& extractor);
    ~SearchSession();

    SearchSession(const SearchSession&) = delete;
    SearchSession& operator=(const SearchSession&) = delete;

    // 回呼都在文字執行緒上被呼叫。onPage 只在該頁有命中時才呼叫，
    // 避免 500 頁的文件對 UI 送出 500 次空更新。
    void start(std::string queryUtf8, SearchOptions options, std::int32_t startPage,
               PageCallback onPage, FinishedCallback onFinished);

    // 取消在頁與頁之間生效，最壞情況是多掃一頁。
    void cancel();

    [[nodiscard]] bool isRunning() const noexcept;

private:
    struct State;

    // 排下一頁的工作。以 shared_ptr 持有狀態，session 先消失時飛在半空的工作仍安全。
    static void scheduleNext(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
};

}  // namespace alioth::engine::text
