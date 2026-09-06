#include "app/selection_controller.h"

#include <QMetaObject>

#include <algorithm>
#include <map>
#include <vector>

namespace alioth::app {

SelectionController::SelectionController(QObject* parent)
    : QObject(parent),
      extractor_(std::make_unique<engine::text::TextExtractor>()),
      search_(std::make_unique<engine::text::SearchSession>(*extractor_)),
      indexBuilder_(std::make_unique<engine::text::TextIndexBuilder>(*extractor_)) {}

SelectionController::~SelectionController() {
    // 這兩者都持有擷取器的參照，必須先於它消失。
    indexBuilder_.reset();
    search_.reset();
    // 文字執行緒的工作會碰本物件的成員。先關掉擷取器（解構會 join 那條執行緒），
    // 其餘成員才安全——與 DocumentController 同樣的理由。
    extractor_.reset();
}

void SelectionController::openDocument(const QString& path, const QString& password) {
    clearSelection();
    pending_ = PendingSearch{};
    path_ = path;
    password_ = password;
    open_ = false;
    // 換文件就丟掉舊索引。留著的話，新文件搜尋會搜到舊文件的內容，
    // 而頁碼還是對得上的——結果看起來完全合理，只是全都是錯的。
    if (indexBuilder_) indexBuilder_->cancel();
    index_.clear();
    extractor_->open(path.toStdString(), password.toStdString(),
                     [this](domain::DocumentError error) {
                         const bool ok = error == domain::DocumentError::None;
                         QMetaObject::invokeMethod(
                             this,
                             [this, ok, error] {
                                 open_ = ok;
                                 emit documentReady(ok, static_cast<int>(error));
                                 if (ok) startIndexing();
                                 if (ok && pending_.valid) {
                                     const PendingSearch request = pending_;
                                     pending_ = PendingSearch{};
                                     search(request.query, request.startPage, request.matchCase,
                                            request.matchWholeWord);
                                 }
                             },
                             Qt::QueuedConnection);
                     });
}

void SelectionController::closeDocument() {
    clearSelection();
    open_ = false;
    if (indexBuilder_) indexBuilder_->cancel();
    index_.clear();
    extractor_->close();
}

void SelectionController::startIndexing() {
    if (!indexBuilder_) return;
    const std::int32_t pageCount = extractor_->pageCount();
    if (pageCount <= 0) return;

    indexBuilder_->start(index_, pageCount, [this](engine::text::IndexProgress progress) {
        // 回呼在文字執行緒上。訊號一律排回 GUI 執行緒才發，
        // 否則接收端會在錯誤的執行緒上碰 UI。
        QMetaObject::invokeMethod(
            this,
            [this, progress] {
                emit searchIndexProgress(progress.indexedPages, progress.totalPages);
            },
            Qt::QueuedConnection);
    });
}

bool SelectionController::indexIsComplete() const {
    return open_ && extractor_->pageCount() > 0 &&
           index_.indexedPageCount() >= extractor_->pageCount();
}

void SelectionController::beginSelection(std::int32_t pageIndex, const domain::PointF& pagePoint) {
    // 這裡刻意不檢查 open_：開檔是非同步的，而 open_ 要等回呼排回 GUI 執行緒才會變 true。
    // 在那之前擋掉請求，等於把「開檔後立刻選字」這個完全正常的操作靜默吃掉。
    // 文字執行緒的佇列本身就保證工作排在開檔之後，順序不需要在這裡再保證一次。
    anchorPage_ = pageIndex;
    anchorChar_ = -1;

    extractor_->withTextPage(pageIndex, [this, pageIndex, pagePoint](
                                            const engine::text::TextPage* page) {
        if (!page) return;
        const std::int32_t index =
            engine::text::charIndexAt(*page, pagePoint, kHitTolerancePt);
        QMetaObject::invokeMethod(
            this,
            [this, pageIndex, index] {
                anchorChar_ = index;
                if (index < 0) {
                    clearSelection();
                } else {
                    updateSelection(pageIndex, domain::TextRange{index, index + 1});
                }
            },
            Qt::QueuedConnection);
    });
}

void SelectionController::mirrorSelectionSummary() {
    // 既有的單頁呼叫端讀的是這三個欄位。跨頁時它們描述第一頁，而 text 是
    // 全部串起來的——複製到剪貼簿的人要的是完整內容，不是第一頁而已。
    if (selection_.pages.empty()) {
        selection_ = Selection{};
        return;
    }
    const PageSelection& first = selection_.pages.front();
    selection_.pageIndex = first.pageIndex;
    selection_.range = first.range;
    selection_.quads = first.quads;

    QString joined;
    for (std::size_t i = 0; i < selection_.pages.size(); ++i) {
        // 頁與頁之間插入換行。不插的話，前一頁最後一個字會與下一頁第一個字
        // 黏在一起，貼出來變成一個不存在的詞。
        if (i > 0) joined += QLatin1Char('\n');
        joined += selection_.pages[i].text;
    }
    selection_.text = std::move(joined);
}

void SelectionController::extendSelectionAcrossPages(std::int32_t endPage, std::int32_t endChar) {
    // 跨頁選取的三段結構：
    //   起始頁  從錨點到該頁結尾
    //   中間頁  整頁
    //   結束頁  從開頭到游標
    // 反向拖曳（由後往前選）時起訖對調，否則使用者只能往後選。
    const std::int32_t firstPage = std::min(anchorPage_, endPage);
    const std::int32_t lastPage = std::max(anchorPage_, endPage);
    const bool forward = endPage >= anchorPage_;

    struct Gather {
        std::map<std::int32_t, PageSelection> pages;
        std::int32_t remaining{0};
    };
    auto gather = std::make_shared<Gather>();
    gather->remaining = lastPage - firstPage + 1;

    for (std::int32_t page = firstPage; page <= lastPage; ++page) {
        extractor_->withTextPage(page, [this, gather, page, firstPage, lastPage, forward,
                                        endChar](const engine::text::TextPage* textPage) {
            PageSelection selection;
            selection.pageIndex = page;
            if (textPage != nullptr) {
                const domain::TextRange full = engine::text::pageRange(*textPage);
                domain::TextRange range = full;
                if (page == firstPage) range.start = forward ? anchorChar_ : endChar;
                if (page == lastPage) range.end = (forward ? endChar : anchorChar_) + 1;
                range = range.clamped(full.end);
                if (!range.isEmpty()) {
                    selection.range = range;
                    selection.quads = engine::text::quadsForRange(*textPage, range);
                    selection.text =
                        QString::fromStdString(engine::text::textForRange(*textPage, range));
                }
            }

            QMetaObject::invokeMethod(
                this,
                [this, gather, selection = std::move(selection)]() mutable {
                    if (!selection.quads.empty()) {
                        gather->pages.emplace(selection.pageIndex, std::move(selection));
                    }
                    if (--gather->remaining > 0) return;

                    // 全部頁面都回來了才更新一次。逐頁更新會讓拖曳過程中
                    // 每經過一頁就閃一次不完整的選取。
                    //
                    // 用 map 收集，所以輸出永遠依頁碼排序，與拖曳方向無關——
                    // 不排序的話，複製出來的文字順序會跟著手勢方向倒過來。
                    selection_.pages.clear();
                    for (auto& entry : gather->pages) {
                        selection_.pages.push_back(std::move(entry.second));
                    }
                    mirrorSelectionSummary();
                    emit selectionChanged();
                },
                Qt::QueuedConnection);
        });
    }
}

void SelectionController::extendSelection(std::int32_t pageIndex, const domain::PointF& pagePoint) {
    if (anchorChar_ < 0) return;

    if (pageIndex != anchorPage_) {
        // 跨頁（PRD-TXT-002）。先問出游標落在這一頁的哪個字元，再組裝整段。
        extractor_->withTextPage(
            pageIndex, [this, pageIndex, pagePoint](const engine::text::TextPage* page) {
                if (page == nullptr) return;
                std::int32_t index =
                    engine::text::charIndexAt(*page, pagePoint, kHitTolerancePt * 4.0);
                if (index < 0) {
                    // 游標落在頁面空白處（例如拖到頁面下緣）時取整頁作為結尾。
                    // 那是使用者的意圖——把游標拖過整頁應該選到整頁，
                    // 而不是因為指到空白就什麼都不選。
                    index = std::max(0, engine::text::charCount(*page) - 1);
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, pageIndex, index] {
                        if (anchorChar_ < 0) return;
                        extendSelectionAcrossPages(pageIndex, index);
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    extractor_->withTextPage(pageIndex, [this, pageIndex, pagePoint](
                                            const engine::text::TextPage* page) {
        if (!page) return;
        const std::int32_t index =
            engine::text::charIndexAt(*page, pagePoint, kHitTolerancePt * 4.0);
        QMetaObject::invokeMethod(
            this,
            [this, pageIndex, index] {
                if (index < 0 || anchorChar_ < 0) return;
                // 反向拖曳（由後往前選）必須也成立，否則使用者只能單向選取。
                const std::int32_t from = std::min(anchorChar_, index);
                const std::int32_t to = std::max(anchorChar_, index) + 1;
                updateSelection(pageIndex, domain::TextRange{from, to});
            },
            Qt::QueuedConnection);
    });
}

void SelectionController::selectWordAt(std::int32_t pageIndex, const domain::PointF& pagePoint) {
    extractor_->withTextPage(pageIndex, [this, pageIndex, pagePoint](
                                            const engine::text::TextPage* page) {
        if (!page) return;
        const std::int32_t index =
            engine::text::charIndexAt(*page, pagePoint, kHitTolerancePt);
        if (index < 0) return;
        const domain::TextRange range = engine::text::wordRangeAt(*page, index);
        QMetaObject::invokeMethod(
            this, [this, pageIndex, range] { updateSelection(pageIndex, range); },
            Qt::QueuedConnection);
    });
}

void SelectionController::selectLineAt(std::int32_t pageIndex, const domain::PointF& pagePoint) {
    extractor_->withTextPage(pageIndex, [this, pageIndex, pagePoint](
                                            const engine::text::TextPage* page) {
        if (!page) return;
        const std::int32_t index =
            engine::text::charIndexAt(*page, pagePoint, kHitTolerancePt);
        if (index < 0) return;
        const domain::TextRange range = engine::text::lineRangeAt(*page, index);
        QMetaObject::invokeMethod(
            this, [this, pageIndex, range] { updateSelection(pageIndex, range); },
            Qt::QueuedConnection);
    });
}

void SelectionController::selectAllOnPage(std::int32_t pageIndex) {
    extractor_->withTextPage(pageIndex, [this, pageIndex](const engine::text::TextPage* page) {
        if (!page) return;
        const domain::TextRange range = engine::text::pageRange(*page);
        if (range.isEmpty()) return;  // 沒有文字的頁面（純掃描件）不該清掉既有選取
        QMetaObject::invokeMethod(
            this, [this, pageIndex, range] { updateSelection(pageIndex, range); },
            Qt::QueuedConnection);
    });
}

void SelectionController::updateSelection(std::int32_t pageIndex, domain::TextRange range) {
    if (range.isEmpty()) {
        clearSelection();
        return;
    }

    // quad 與文字都要回文字執行緒才拿得到，所以這裡再繞一趟。
    extractor_->withTextPage(pageIndex, [this, pageIndex, range](
                                            const engine::text::TextPage* page) {
        if (!page) return;
        auto quads = engine::text::quadsForRange(*page, range);
        auto text = engine::text::textForRange(*page, range);
        QMetaObject::invokeMethod(
            this,
            [this, pageIndex, range, quads = std::move(quads), text = std::move(text)]() mutable {
                PageSelection page;
                page.pageIndex = pageIndex;
                page.range = range;
                page.quads = std::move(quads);
                page.text = QString::fromStdString(text);

                selection_.pages.clear();
                selection_.pages.push_back(std::move(page));
                mirrorSelectionSummary();
                emit selectionChanged();
            },
            Qt::QueuedConnection);
    });
}

void SelectionController::search(const QString& query, std::int32_t startPage, bool matchCase,
                                 bool matchWholeWord) {
    cancelSearch();
    hits_.clear();
    emit searchHitsChanged();

    if (query.isEmpty()) {
        emit searchFinished(0, false);
        return;
    }

    if (!open_) {
        pending_ = PendingSearch{query, startPage, matchCase, matchWholeWord, true};
        return;
    }

    engine::text::SearchOptions options;
    options.matchCase = matchCase;
    options.matchWholeWord = matchWholeWord;

    // 索引建完就走索引（ADR-005）：500 頁工程圖從 6688 毫秒降到 4.2 毫秒，
    // 而且完全不碰 PDFium，所以不會和渲染搶那條執行緒。
    //
    // 索引沒建完就退回逐頁搜尋。慢，但**正確**——拿一份只有一半的索引去搜，
    // 使用者看到的是「找不到」而不是「還在建索引」，那是搜尋最不能犯的錯。
    if (indexIsComplete()) {
        const auto results = index_.search(query.toStdString(), options, startPage);
        // 逐頁回報是為了讓 500 頁的搜尋不必等整份掃完；索引搜尋本來就是一次算完的，
        // 這裡仍然照頁分組送出，讓接收端不必分辨結果是哪條路徑來的。
        std::int32_t currentPage = -1;
        std::vector<domain::SearchResult> batch;
        const auto flush = [this, &currentPage, &batch] {
            if (batch.empty()) return;
            appendHits(currentPage, batch);
            batch.clear();
        };
        for (const domain::SearchResult& result : results) {
            if (result.pageIndex != currentPage) {
                flush();
                currentPage = result.pageIndex;
            }
            batch.push_back(result);
        }
        flush();
        emit searchHitsChanged();
        emit searchFinished(static_cast<int>(results.size()), false);
        return;
    }

    search_->start(
        query.toStdString(), options, startPage,
        [this](std::int32_t pageIndex, std::vector<domain::SearchResult> results) {
            QMetaObject::invokeMethod(
                this,
                [this, pageIndex, results = std::move(results)] {
                    appendHits(pageIndex, results);
                    emit searchHitsChanged();
                },
                Qt::QueuedConnection);
        },
        [this](engine::text::SearchSummary summary) {
            QMetaObject::invokeMethod(
                this,
                [this, summary] { emit searchFinished(summary.totalMatches, summary.cancelled); },
                Qt::QueuedConnection);
        });
}

void SelectionController::appendHits(std::int32_t pageIndex,
                                     const std::vector<domain::SearchResult>& results) {
    for (const domain::SearchResult& result : results) {
        SearchHit hit;
        hit.pageIndex = pageIndex;
        hit.range = result.range;
        hit.context = QString::fromStdString(result.context);
        hit.matchOffset = result.matchOffset;
        hit.matchLength = result.matchLength;
        hits_.push_back(std::move(hit));
    }
}

void SelectionController::cancelSearch() {
    if (search_) search_->cancel();
}

void SelectionController::selectSearchHit(std::size_t index) {
    if (index >= hits_.size()) return;
    const SearchHit& hit = hits_[index];
    updateSelection(hit.pageIndex, hit.range);
}

void SelectionController::clearSelection() {
    if (selection_.isEmpty() && selection_.pageIndex < 0) return;
    selection_ = Selection{};
    anchorChar_ = -1;
    anchorPage_ = -1;
    emit selectionChanged();
}

}  // namespace alioth::app
