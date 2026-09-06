// 多文件搜尋測試（PRD-SRCH-002）——逐份序列處理。
//
// 見 engine/text/multi_document_search.h 檔頭：這裡刻意不驗證「並行」，
// 因為架構上就沒有並行——驗證的是「多份文件依序被搜尋、結果正確歸屬到各自
// 的文件與頁碼、開檔失敗的文件被跳過但不中止整次搜尋、取消可以生效」。

#include <QtTest>

#include <map>
#include <string>
#include <vector>

#include "engine/text/multi_document_search.h"
#include "pdf_fixture.h"
#include "text_pdf_fixture.h"

using namespace alioth::domain;
using namespace alioth::engine::text;

class TestMultiDocumentSearch : public QObject {
    Q_OBJECT

private slots:
    void searchesEveryDocumentSequentiallyAndReportsPerDocumentResults() {
        auto fileA = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        auto fileB = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        QVERIFY(fileA != nullptr);
        QVERIFY(fileB != nullptr);

        MultiDocumentSearch session;
        MultiDocumentQuery query;
        query.paths = {fileA->fileName().toStdString(), fileB->fileName().toStdString()};
        query.queryUtf8 = "Alioth";

        std::vector<std::size_t> openedOrder;
        std::map<std::size_t, int> hitsPerDocument;
        MultiDocumentStats stats{};
        bool finished = false;

        session.start(
            std::move(query),
            [&](std::size_t docIndex, DocumentError error) {
                QCOMPARE(error, DocumentError::None);
                openedOrder.push_back(docIndex);
            },
            [&](std::size_t docIndex, std::int32_t /*pageIndex*/,
                std::vector<SearchResult> results) {
                hitsPerDocument[docIndex] += static_cast<int>(results.size());
            },
            [&](MultiDocumentStats s) {
                stats = s;
                finished = true;
            });
        session.waitForIdle();

        QVERIFY(finished);
        QCOMPARE(stats.documentsSearched, std::int32_t{2});
        QCOMPARE(stats.documentsSkipped, std::int32_t{0});
        QVERIFY(!stats.cancelled);
        // 兩份文件都要依序被開啟過，順序就是 paths 的順序——這是「逐份序列」
        // 這個設計選擇本身可觀察到的行為，不是巧合。
        QCOMPARE(openedOrder, (std::vector<std::size_t>{0, 1}));
        // 兩份文件內容相同（makeTextPdf 產生的兩頁都含 "Alioth"），各自應有命中。
        QVERIFY(hitsPerDocument[0] > 0);
        QVERIFY(hitsPerDocument[1] > 0);
        QCOMPARE(stats.totalMatches, hitsPerDocument[0] + hitsPerDocument[1]);
    }

    void skipsUnopenableDocumentsWithoutFailingTheWholeSearch() {
        auto fileA = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        QVERIFY(fileA != nullptr);

        MultiDocumentSearch session;
        MultiDocumentQuery query;
        query.paths = {"C:/this/path/does/not/exist.pdf", fileA->fileName().toStdString()};
        query.queryUtf8 = "Alioth";

        MultiDocumentStats stats{};
        std::vector<DocumentError> openErrors;
        session.start(
            std::move(query),
            [&](std::size_t /*docIndex*/, DocumentError error) { openErrors.push_back(error); },
            [](std::size_t, std::int32_t, std::vector<SearchResult>) {},
            [&](MultiDocumentStats s) { stats = s; });
        session.waitForIdle();

        QCOMPARE(stats.documentsSkipped, std::int32_t{1});
        QCOMPARE(stats.documentsSearched, std::int32_t{1});
        QCOMPARE(openErrors.size(), std::size_t{2});
        QVERIFY(openErrors[0] != DocumentError::None);
        QCOMPARE(openErrors[1], DocumentError::None);
    }

    void cancelStopsBeforeRemainingDocuments() {
        auto fileA = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        auto fileB = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        auto fileC = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        QVERIFY(fileA && fileB && fileC);

        MultiDocumentSearch session;
        MultiDocumentQuery query;
        query.paths = {fileA->fileName().toStdString(), fileB->fileName().toStdString(),
                       fileC->fileName().toStdString()};
        query.queryUtf8 = "Alioth";

        MultiDocumentStats stats{};
        session.start(
            std::move(query), [&](std::size_t docIndex, DocumentError) {
                // 開完第一份就取消：後面的文件不該再被開啟。
                if (docIndex == 0) session.cancel();
            },
            [](std::size_t, std::int32_t, std::vector<SearchResult>) {},
            [&](MultiDocumentStats s) { stats = s; });
        session.waitForIdle();

        QVERIFY(stats.cancelled);
        // 取消發生在第一份文件的開檔回呼裡（早於任何頁面被排入），因此只有
        // 第一份被計為已搜尋，後兩份完全沒被開啟。
        QCOMPARE(stats.documentsSearched, std::int32_t{1});
    }
};

QTEST_APPLESS_MAIN(TestMultiDocumentSearch)
#include "test_multi_document_search.moc"
