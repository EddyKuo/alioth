#pragma once

// 並行全文搜尋（PRD-SRCH-001：500 頁 ≤ 2 秒）。
//
// **這條路已經否決，不要接線，也不要照這個形狀再寫一個（ADR-005）。**
//
// 隔離實驗的結論是：PDFium 的文件把手**可以並存，不可以並行**。
// 四個擷取器同時活著、但嚴格輪流動作時完全正常（每個 10/10）；
// 一旦兩條執行緒同時動作，就會非決定性地掉資料——同一組設定第一輪 60/60、
// 第二輪 5/60，之後整個行程的開檔開始失敗。量測表見 ADR-005。
//
// 本檔保留只為了讓「為什麼不這樣做」有實體可指（本專案無版本控制，刪掉就沒了）。
// PRD-SRCH-001 的 2 秒預算改由單一擷取器預建文字索引達成，完全不需要並行。
//
// 存在理由是一個量出來的事實：單執行緒搜尋 500 頁 A0 工程圖語料要 3874 毫秒，
// 預算是 2000 毫秒。成本不在比對字串，而在 PDFium 每頁都得把含數千條向量的
// 內容串流解析一遍才拿得到文字層——那是 CPU-bound 且逐頁獨立的工作。
//
// PDFium 非執行緒安全，所以加速的方式不是把一份文件丟給多條執行緒，而是開 N 個
// TextExtractor：每個持有自己的文件把手與專用執行緒，各自負責一段頁碼。
// 這正是 CLAUDE.md 與 SDD §1.1 指定的合法並行方式。
//
// 代價是 N 份文件把手的記憶體。因此 N 有上限，而且對小文件不值得——
// 開把手本身要成本，頁數少時單執行緒反而快。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/text_layer.h"
#include "engine/text/text_search.h"

namespace alioth::engine::text {

// 低於這個頁數就走單執行緒：開 N 份把手的固定成本會蓋過並行的收益。
inline constexpr std::int32_t kParallelSearchMinPages = 24;

// 併發上限。超過這個數字後瓶頸從 CPU 轉到記憶體與檔案 I/O，
// 而每多一份把手就多一份文件結構的常駐記憶體。
inline constexpr int kMaxSearchWorkers = 4;

struct ParallelSearchStats {
    int workers{0};
    std::int32_t pagesScanned{0};
    std::int32_t totalMatches{0};
    bool cancelled{false};
};

// 跨頁並行搜尋。與 SearchSession 的介面刻意相近，讓呼叫端可以依文件大小切換。
class ParallelSearchSession {
public:
    using PageCallback =
        std::function<void(std::int32_t pageIndex, std::vector<domain::SearchResult>)>;
    using FinishedCallback = std::function<void(ParallelSearchStats)>;

    ParallelSearchSession();
    ~ParallelSearchSession();

    ParallelSearchSession(const ParallelSearchSession&) = delete;
    ParallelSearchSession& operator=(const ParallelSearchSession&) = delete;

    // path 會被每個工作者各自開啟一次。開檔失敗的工作者會被略過而不是讓整次搜尋失敗——
    // 少一個工作者只是慢一點，讓整次搜尋失敗才是真的壞掉。
    void start(std::string path, std::string password, std::string queryUtf8, SearchOptions options,
               std::int32_t pageCount, PageCallback onPage, FinishedCallback onFinished);

    void cancel();
    [[nodiscard]] bool isRunning() const;

    // 同步等待，僅供測試使用。
    void waitForIdle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 依頁數決定該用幾個工作者。頁數不足時回傳 1，呼叫端據此決定走哪條路。
[[nodiscard]] int recommendedWorkerCount(std::int32_t pageCount);

}  // namespace alioth::engine::text
