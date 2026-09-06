#include "engine/text/multi_document_search.h"

#include <atomic>
#include <utility>

#include "engine/text/text_extractor.h"

namespace alioth::engine::text {

struct MultiDocumentSearch::Impl {
    std::unique_ptr<TextExtractor> extractor{std::make_unique<TextExtractor>()};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{false};

    MultiDocumentQuery query;
    DocumentCallback onDocumentOpened;
    PageCallback onPage;
    FinishedCallback onFinished;
    MultiDocumentStats stats;

    void finish() {
        running.store(false);
        stats.cancelled = cancelled.load();
        FinishedCallback callback = std::move(onFinished);
        onFinished = {};
        if (callback) callback(stats);
    }

    void processPage(std::size_t docIndex, std::int32_t page, std::int32_t pageCount) {
        if (cancelled.load() || page >= pageCount) {
            processDocument(docIndex + 1);
            return;
        }
        extractor->withTextPage(page, [this, docIndex, page, pageCount](const TextPage* textPage) {
            if (textPage != nullptr) {
                std::vector<domain::SearchResult> results =
                    searchPage(*textPage, query.queryUtf8, query.options);
                ++stats.pagesScanned;
                if (!results.empty()) {
                    stats.totalMatches += static_cast<std::int32_t>(results.size());
                    if (onPage) onPage(docIndex, page, std::move(results));
                }
            }
            processPage(docIndex, page + 1, pageCount);
        });
    }

    void processDocument(std::size_t index) {
        if (cancelled.load() || index >= query.paths.size()) {
            finish();
            return;
        }
        extractor->open(query.paths[index], query.password,
                        [this, index](domain::DocumentError error) {
                            if (error != domain::DocumentError::None) {
                                // 開檔失敗只跳過這一份，不讓整次搜尋失敗——少一份文件的結果
                                // 只是不完整，讓整次搜尋失敗才是真的壞掉（IL-4 的同一個原則）。
                                ++stats.documentsSkipped;
                                if (onDocumentOpened) onDocumentOpened(index, error);
                                processDocument(index + 1);
                                return;
                            }
                            ++stats.documentsSearched;
                            if (onDocumentOpened) onDocumentOpened(index, domain::DocumentError::None);
                            const std::int32_t pageCount = extractor->pageCount();
                            processPage(index, 0, pageCount);
                        });
    }
};

MultiDocumentSearch::MultiDocumentSearch() : impl_(std::make_unique<Impl>()) {}

MultiDocumentSearch::~MultiDocumentSearch() {
    cancel();
    // TextExtractor 的解構會 join 它的執行緒，因此這裡不需要額外等待——
    // 只要不再有新工作被排入（cancel 已經確保），佇列會自然清空。
}

void MultiDocumentSearch::start(MultiDocumentQuery query, DocumentCallback onDocumentOpened,
                                PageCallback onPage, FinishedCallback onFinished) {
    impl_->cancelled.store(false);
    impl_->running.store(true);
    impl_->query = std::move(query);
    impl_->onDocumentOpened = std::move(onDocumentOpened);
    impl_->onPage = std::move(onPage);
    impl_->onFinished = std::move(onFinished);
    impl_->stats = MultiDocumentStats{};
    impl_->processDocument(0);
}

void MultiDocumentSearch::cancel() { impl_->cancelled.store(true); }

bool MultiDocumentSearch::isRunning() const noexcept { return impl_->running.load(); }

void MultiDocumentSearch::waitForIdle() { impl_->extractor->waitForIdle(); }

}  // namespace alioth::engine::text
