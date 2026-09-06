// 全文搜尋測試（WBS 2.9，PRD-SRCH-001）。
//
// 兩個層次分開驗：單頁 searchPage 驗旗標與上下文，SearchSession 驗逐頁增量與取消。

#include <QtTest>

#include <map>
#include <string>
#include <vector>

#include "engine/text/text_extractor.h"
#include "engine/text/text_search.h"
#include "pdf_fixture.h"
#include "text_pdf_fixture.h"

using namespace alioth::domain;
using namespace alioth::engine::text;

namespace {

// 見 test_text_extractor.cpp 的同名工具：把文字執行緒上的工作變成同步呼叫。
template <typename Fn>
void onTextPage(TextExtractor& extractor, std::int32_t pageIndex, Fn&& fn) {
    extractor.withTextPage(pageIndex, [&fn](const TextPage* page) { fn(page); });
    extractor.waitForIdle();
}

}  // namespace

class TestTextSearch : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        file_ = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        QVERIFY2(file_ != nullptr, "無法建立測試用 PDF");

        DocumentError error = DocumentError::Unknown;
        extractor_.open(file_->fileName().toStdString(), "",
                        [&error](DocumentError e) { error = e; });
        extractor_.waitForIdle();
        QCOMPARE(error, DocumentError::None);
    }

    void findsEveryOccurrenceOnAPage() {
        std::vector<SearchResult> hits;
        std::vector<std::string> matched;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            hits = searchPage(*page, "Alioth", SearchOptions{});
            for (const SearchResult& hit : hits) {
                matched.push_back(textForRange(*page, hit.range));
            }
        });

        QCOMPARE(hits.size(), std::size_t(2));
        for (const std::string& text : matched) {
            // 結果索引若有偏移，這裡就會拿到 "lioth " 之類的字串。
            QCOMPARE(text, std::string("Alioth"));
        }
        QVERIFY(hits[0].range.start < hits[1].range.start);
        QCOMPARE(hits[0].range.count(), 6);
        QCOMPARE(hits[0].pageIndex, 0);
    }

    void missingTermReturnsEmpty() {
        std::vector<SearchResult> hits;
        std::vector<SearchResult> emptyQuery;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            hits = searchPage(*page, "zzz-not-here", SearchOptions{});
            emptyQuery = searchPage(*page, "", SearchOptions{});
        });
        QVERIFY(hits.empty());
        QVERIFY(emptyQuery.empty());
    }

    void matchCaseFiltersResults() {
        std::size_t insensitive = 0;
        std::size_t sensitive = 0;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            SearchOptions options;
            insensitive = searchPage(*page, "alioth", options).size();
            options.matchCase = true;
            sensitive = searchPage(*page, "alioth", options).size();
        });
        QCOMPARE(insensitive, std::size_t(2));
        QCOMPARE(sensitive, std::size_t(0));
    }

    void wholeWordFiltersSubstrings() {
        std::size_t loose = 0;
        std::size_t whole = 0;
        std::size_t wholeWordExact = 0;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            SearchOptions options;
            loose = searchPage(*page, "lio", options).size();
            options.matchWholeWord = true;
            whole = searchPage(*page, "lio", options).size();
            wholeWordExact = searchPage(*page, "line", options).size();
        });
        QCOMPARE(loose, std::size_t(2));
        QCOMPARE(whole, std::size_t(0));
        QCOMPARE(wholeWordExact, std::size_t(2));
    }

    void resultsCarryContext() {
        SearchResult hit{};
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            const auto hits = searchPage(*page, "mentions", SearchOptions{});
            QVERIFY(!hits.empty());
            hit = hits.front();
        });

        QVERIFY(!hit.context.empty());
        QCOMPARE(hit.matchLength, 8);
        QVERIFY(hit.matchOffset >= 0);
        QVERIFY(static_cast<std::size_t>(hit.matchOffset + hit.matchLength) <= hit.context.size());
        QCOMPARE(hit.context.substr(static_cast<std::size_t>(hit.matchOffset),
                                    static_cast<std::size_t>(hit.matchLength)),
                 std::string("mentions"));
        QVERIFY2(hit.context.find("Third line") != std::string::npos, "上下文沒帶到命中前的文字");
        // 上下文是要塞進單行列表的，不得夾帶換行。
        QVERIFY(hit.context.find('\n') == std::string::npos);
        QVERIFY(hit.context.find('\r') == std::string::npos);
    }

    void sessionScansEveryPageIncrementally() {
        SearchSession session(extractor_);
        std::map<std::int32_t, std::size_t> perPage;
        SearchSummary summary{};
        bool finished = false;

        session.start("Alioth", SearchOptions{}, 0,
                      [&perPage](std::int32_t page, std::vector<SearchResult> hits) {
                          perPage[page] = hits.size();
                      },
                      [&summary, &finished](SearchSummary s) {
                          summary = s;
                          finished = true;
                      });
        extractor_.waitForIdle();

        QVERIFY(finished);
        QVERIFY(!summary.cancelled);
        QCOMPARE(summary.pagesScanned, 2);
        QCOMPARE(summary.totalMatches, 3);
        QCOMPARE(perPage[0], std::size_t(2));
        QCOMPARE(perPage[1], std::size_t(1));
        QVERIFY(!session.isRunning());
    }

    void sessionStartsAtCurrentPageAndWrapsAround() {
        SearchSession session(extractor_);
        std::vector<std::int32_t> order;
        bool finished = false;

        session.start("Alioth", SearchOptions{}, 1,
                      [&order](std::int32_t page, std::vector<SearchResult>) {
                          order.push_back(page);
                      },
                      [&finished](SearchSummary) { finished = true; });
        extractor_.waitForIdle();

        QVERIFY(finished);
        // 從第 2 頁開始找，繞回第 1 頁補齊；順序反了代表「找下一個」會跳錯方向。
        const std::vector<std::int32_t> expected{1, 0};
        QCOMPARE(order, expected);
    }

    void cancelledSessionStopsAndReports() {
        SearchSession session(extractor_);
        SearchSummary summary{};
        bool finished = false;

        // 在第一頁的回呼裡取消，避免「主執行緒喊停時搜尋早就跑完」的競態，
        // 讓斷言對「取消在頁與頁之間生效」這件事是確定的。
        session.start("Alioth", SearchOptions{}, 0,
                      [&session](std::int32_t, std::vector<SearchResult>) { session.cancel(); },
                      [&summary, &finished](SearchSummary s) {
                          summary = s;
                          finished = true;
                      });
        extractor_.waitForIdle();

        QVERIFY2(finished, "取消也必須回報結束，否則呼叫端的搜尋面板會卡在忙碌狀態");
        QVERIFY(summary.cancelled);
        QCOMPARE(summary.pagesScanned, 1);
    }

    void emptyQueryFinishesImmediately() {
        SearchSession session(extractor_);
        SearchSummary summary{};
        bool finished = false;
        session.start("", SearchOptions{}, 0, {},
                      [&summary, &finished](SearchSummary s) {
                          summary = s;
                          finished = true;
                      });
        extractor_.waitForIdle();

        QVERIFY(finished);
        QCOMPARE(summary.pagesScanned, 0);
        QCOMPARE(summary.totalMatches, 0);
    }

private:
    std::unique_ptr<QTemporaryFile> file_;
    TextExtractor extractor_;
};

QTEST_APPLESS_MAIN(TestTextSearch)
#include "test_text_search.moc"
