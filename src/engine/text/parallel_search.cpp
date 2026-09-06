#include "engine/text/parallel_search.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "engine/cancellation.h"
#include "engine/text/text_extractor.h"

namespace alioth::engine::text {

int recommendedWorkerCount(std::int32_t pageCount) {
    if (pageCount < kParallelSearchMinPages) return 1;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    // 留一顆核心給 GUI 與渲染：搜尋期間使用者還在捲動，把所有核心吃光會讓畫面卡住，
    // 而使用者對卡頓的容忍度遠低於對搜尋慢的容忍度。
    const int usable = static_cast<int>(hardware > 2 ? hardware - 1 : 1);
    return std::clamp(usable, 1, kMaxSearchWorkers);
}

struct ParallelSearchSession::Impl {
    struct Worker {
        std::unique_ptr<TextExtractor> extractor;
        bool usable{false};
    };

    std::vector<Worker> workers;
    std::mutex mutex;
    std::atomic<int> outstanding{0};
    std::atomic<std::int32_t> pagesScanned{0};
    std::atomic<std::int32_t> totalMatches{0};
    std::atomic<bool> cancelled{false};
    CancellationSource cancellation;
    FinishedCallback onFinished;

    void finishOne() {
        if (outstanding.fetch_sub(1) != 1) return;

        FinishedCallback callback;
        {
            std::lock_guard lock(mutex);
            callback = std::move(onFinished);
            onFinished = {};
        }
        if (!callback) return;

        ParallelSearchStats stats;
        stats.workers = static_cast<int>(workers.size());
        stats.pagesScanned = pagesScanned.load();
        stats.totalMatches = totalMatches.load();
        stats.cancelled = cancelled.load();
        callback(stats);
    }
};

ParallelSearchSession::ParallelSearchSession() : impl_(std::make_unique<Impl>()) {}

ParallelSearchSession::~ParallelSearchSession() {
    cancel();
    // 工作者的解構會 join 各自的執行緒。必須先取消再解構，
    // 否則要等一輪完整的頁面掃描才會結束。
    impl_->workers.clear();
}

void ParallelSearchSession::start(std::string path, std::string password, std::string queryUtf8,
                                  SearchOptions options, std::int32_t pageCount,
                                  PageCallback onPage, FinishedCallback onFinished) {
    cancel();
    impl_->workers.clear();
    impl_->pagesScanned.store(0);
    impl_->totalMatches.store(0);
    impl_->cancelled.store(false);
    impl_->cancellation.reset();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->onFinished = std::move(onFinished);
    }

    if (queryUtf8.empty() || pageCount <= 0) {
        impl_->outstanding.store(1);
        impl_->finishOne();
        return;
    }

    const int workerCount = recommendedWorkerCount(pageCount);
    impl_->workers.resize(static_cast<std::size_t>(workerCount));

    // 先把每個工作者的文件開起來，確定有幾個真的可用，再決定 outstanding。
    // 反過來的話，開檔失敗的工作者會讓計數永遠歸不了零，搜尋看起來像卡死。
    for (Impl::Worker& worker : impl_->workers) {
        worker.extractor = std::make_unique<TextExtractor>();

        std::atomic<bool> opened{false};
        worker.extractor->open(path, password, [&opened](domain::DocumentError error) {
            opened.store(error == domain::DocumentError::None);
        });
        worker.extractor->waitForIdle();
        worker.usable = opened.load();
    }

    std::vector<Impl::Worker*> usable;
    for (Impl::Worker& worker : impl_->workers) {
        if (worker.usable) usable.push_back(&worker);
    }

    if (usable.empty()) {
        impl_->outstanding.store(1);
        impl_->finishOne();
        return;
    }

    // 工作者不能多於頁數：分不到任何頁的工作者永遠不會回報完成，
    // outstanding 就永遠歸不了零，症狀是搜尋看起來卡在最後一頁。
    if (static_cast<std::int32_t>(usable.size()) > pageCount) {
        usable.resize(static_cast<std::size_t>(pageCount));
    }

    const auto stride = static_cast<std::int32_t>(usable.size());
    const CancellationToken token = impl_->cancellation.token();

    // 以「頁」為單位計數而不是以「工作者」：後者要假設「每個工作者的最後一頁最後執行」，
    // 而那個假設一旦被佇列的實作細節打破，完成回呼會在還有頁面沒掃完時就發出——
    // 症狀是搜尋結果少了一截，而且每次執行少的數量都不一樣。
    impl_->outstanding.store(pageCount);

    for (std::size_t i = 0; i < usable.size(); ++i) {
        TextExtractor* extractor = usable[i]->extractor.get();
        const auto offset = static_cast<std::int32_t>(i);

        // 頁面以交錯方式分配而不是切成連續區塊：文件裡文字密度分布不均，
        // 連續切法會讓拿到目錄與空白頁的工作者早早閒置，拿到內文的還在跑。
        //
        // 每一頁各自排一次 withTextPage，工作在該工作者的執行緒上依序執行；
        // 並行發生在工作者之間，不在單一工作者內部——那是 PDFium 的限制決定的。
        for (std::int32_t page = offset; page < pageCount; page += stride) {
            extractor->withTextPage(
                page, [this, page, queryUtf8, options, token, onPage](const TextPage* textPage) {
                    if (token.isCancelled()) {
                        impl_->cancelled.store(true);
                    } else if (textPage) {
                        auto results = searchPage(*textPage, queryUtf8, options, token);
                        impl_->pagesScanned.fetch_add(1);
                        if (!results.empty()) {
                            impl_->totalMatches.fetch_add(static_cast<std::int32_t>(results.size()));
                            if (onPage) onPage(page, std::move(results));
                        }
                    }
                    impl_->finishOne();
                });
        }
    }
}

void ParallelSearchSession::cancel() {
    impl_->cancellation.cancelAll();
    impl_->cancelled.store(true);
}

bool ParallelSearchSession::isRunning() const { return impl_->outstanding.load() > 0; }

void ParallelSearchSession::waitForIdle() {
    for (Impl::Worker& worker : impl_->workers) {
        if (worker.extractor) worker.extractor->waitForIdle();
    }
}

}  // namespace alioth::engine::text
