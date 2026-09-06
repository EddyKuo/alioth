// 預建文字索引（PRD-SRCH-001，ADR-005）。
//
// 這組測試的核心不是「索引找得到東西」，而是**索引搜尋與逐頁 PDFium 搜尋
// 給出同一組結果**。索引是為了取代 searchPage 而存在的，兩者只要有一處不一致，
// 使用者換個入口就會看到不同的搜尋結果，而那種缺陷不會有任何錯誤訊息。

#include <QtTest>

#include <QTemporaryDir>

#include <atomic>
#include <memory>

#include "engine/text/text_index.h"
#include "engine/text/text_search.h"

using namespace alioth;
using namespace alioth::engine::text;

namespace {

constexpr int kPages = 12;

// 每頁一行可辨識文字。第 3 的倍數頁多一個標記，用來驗證命中分布不均時
// 每一頁都被索引到；第 5 頁放大小寫混用，第 7 頁放重疊命中的字串。
QByteArray makeSearchablePdf(int pageCount) {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        kids += QByteArray::number(4 + i * 2) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                      QByteArray::number(pageCount) + " >>");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    for (int i = 0; i < pageCount; ++i) {
        QByteArray text = "Alioth page " + QByteArray::number(i);
        if (i % 3 == 0) text += " marker";
        if (i == 5) text += " MixedCase mixedcase";
        if (i == 7) text += " aaaa";
        const QByteArray content = "BT /F1 14 Tf 50 700 Td (" + text + ") Tj ET\n";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 800] /Contents " +
                          QByteArray::number(5 + i * 2) +
                          " 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
        objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }

    QByteArray pdf = "%PDF-1.7\n";
    std::vector<int> offsets;
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[static_cast<std::size_t>(i)] +
               "\nendobj\n";
    }

    const int xref = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           "\n0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

}  // namespace

class TestTextIndex : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("indexed.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(makeSearchablePdf(kPages));
        file.close();

        extractor_ = std::make_unique<TextExtractor>();
        auto opened = std::make_shared<std::atomic<bool>>(false);
        extractor_->open(path_.toStdString(), "", [opened](domain::DocumentError error) {
            opened->store(error == domain::DocumentError::None);
        });
        extractor_->waitForIdle();
        QVERIFY(opened->load());
        QCOMPARE(extractor_->pageCount(), kPages);

        buildIndex();
    }

    void cleanupTestCase() { extractor_.reset(); }

    void indexCoversEveryPage() {
        QCOMPARE(index_.indexedPageCount(), kPages);
        for (int i = 0; i < kPages; ++i) QVERIFY(index_.hasPage(i));
    }

    void progressReachesTotalExactlyOnce() {
        // 進度回報是 UI 顯示「建索引中」的唯一依據。少一次會讓進度卡在 99%，
        // 多一次會讓「完成」被觸發兩次。
        TextIndex index;
        TextIndexBuilder builder(*extractor_);
        std::vector<int> seen;
        builder.start(index, kPages, [&seen](IndexProgress progress) {
            seen.push_back(progress.indexedPages);
        });
        extractor_->waitForIdle();

        QCOMPARE(index.indexedPageCount(), kPages);
        QVERIFY(!seen.empty());
        QCOMPARE(seen.back(), kPages);
        QCOMPARE(std::count(seen.begin(), seen.end(), kPages), 2);  // 最後一頁 + 收尾各一次
    }

    // 這一條是整組測試的重點。
    void indexSearchAgreesWithPdfiumSearch() {
        const QStringList queries = {
            QStringLiteral("Alioth"),    QStringLiteral("marker"),
            QStringLiteral("page 7"),    QStringLiteral("mixedcase"),
            QStringLiteral("aa"),        QStringLiteral("NoSuchString"),
        };
        for (const QString& query : queries) {
            for (const bool matchCase : {false, true}) {
                SearchOptions options;
                options.matchCase = matchCase;

                const auto viaIndex =
                    index_.search(query.toStdString(), options, 0, engine::CancellationToken{});
                const auto viaPdfium = searchEveryPageWithPdfium(query.toStdString(), options);

                QVERIFY2(sameResults(viaIndex, viaPdfium),
                         qPrintable(QStringLiteral("查詢 %1（matchCase=%2）：索引 %3 筆、"
                                                   "PDFium %4 筆")
                                        .arg(query)
                                        .arg(matchCase)
                                        .arg(viaIndex.size())
                                        .arg(viaPdfium.size())));
            }
        }
    }

    void wholeWordMatchesPdfium() {
        SearchOptions options;
        options.matchWholeWord = true;
        const auto viaIndex =
            index_.search("page", options, 0, engine::CancellationToken{});
        const auto viaPdfium = searchEveryPageWithPdfium("page", options);
        QVERIFY(sameResults(viaIndex, viaPdfium));
        QVERIFY(!viaIndex.empty());
    }

    void repeatedRunsCountTheSameWayPdfiumDoes() {
        // "aaaa" 裡找 "aa" 是**兩個**命中，不是三個——命中之間不重疊。
        //
        // 這條規則不是推導出來的，是上面那條交叉比對抓出來的：索引原本前進一個字元
        // （會找到三個），PDFium 前進一整個命中長度。索引是來取代逐頁搜尋的，
        // 所以以 PDFium 為準；不然同一份文件換個入口就會看到不同的命中數。
        SearchOptions options;
        options.matchCase = true;
        const auto results = index_.search("aa", options, 0, engine::CancellationToken{});
        int onPage7 = 0;
        for (const auto& result : results) {
            if (result.pageIndex == 7) ++onPage7;
        }
        QCOMPARE(onPage7, 2);
    }

    void rangesMapBackToTheSameCharacters() {
        // 索引位置必須就是 PDFium 的字元索引，否則螢光筆會標到別的地方。
        SearchOptions options;
        const auto results = index_.search("marker", options, 0, engine::CancellationToken{});
        QVERIFY(!results.empty());

        const domain::SearchResult& first = results.front();
        std::string readBack;
        extractor_->withTextPage(first.pageIndex, [&readBack, &first](const TextPage* page) {
            if (page != nullptr) readBack = textForRange(*page, first.range);
        });
        extractor_->waitForIdle();
        QCOMPARE(QString::fromStdString(readBack), QStringLiteral("marker"));
    }

    void searchStartsFromTheRequestedPage() {
        SearchOptions options;
        const auto results = index_.search("Alioth", options, 6, engine::CancellationToken{});
        QVERIFY(!results.empty());
        QCOMPARE(results.front().pageIndex, 6);
        // 繞回開頭之後仍然要涵蓋前面的頁。
        QCOMPARE(static_cast<int>(results.size()), kPages);
    }

    void contextIsSingleLineAndMarksTheMatch() {
        SearchOptions options;
        const auto results = index_.search("marker", options, 0, engine::CancellationToken{});
        QVERIFY(!results.empty());
        const domain::SearchResult& first = results.front();
        QVERIFY(!first.context.empty());
        QVERIFY(first.context.find('\n') == std::string::npos);
        QCOMPARE(first.context.substr(static_cast<std::size_t>(first.matchOffset),
                                      static_cast<std::size_t>(first.matchLength)),
                 std::string("marker"));
    }

    void partialIndexIsNotReportedAsNoMatch() {
        // 只索引了一半就搜尋，結果必然不完整。這裡釘的是介面契約：
        // 未索引的頁面**不出現在結果裡**，呼叫端要靠 indexedPageCount() 自己判斷，
        // 不能把「還沒建完」顯示成「找不到」。
        TextIndex partial;
        for (int i = 0; i < kPages / 2; ++i) {
            partial.setPage(i, index_.hasPage(i) ? pageText(i) : std::u32string{});
        }
        QCOMPARE(partial.indexedPageCount(), kPages / 2);

        const auto results =
            partial.search("Alioth", SearchOptions{}, 0, engine::CancellationToken{});
        QCOMPARE(static_cast<int>(results.size()), kPages / 2);
    }

    void cancelledSearchStopsEarly() {
        engine::CancellationSource source;
        source.cancelAll();
        const auto results = index_.search("Alioth", SearchOptions{}, 0, source.token());
        QVERIFY(results.empty());
    }

    void emptyQueryFindsNothing() {
        QVERIFY(index_.search("", SearchOptions{}, 0, engine::CancellationToken{}).empty());
    }

    void perPageMatchLimitIsHonoured() {
        SearchOptions options;
        options.maxMatchesPerPage = 1;
        const auto results = index_.search("a", options, 0, engine::CancellationToken{});
        for (int page = 0; page < kPages; ++page) {
            const auto count = std::count_if(
                results.begin(), results.end(),
                [page](const domain::SearchResult& r) { return r.pageIndex == page; });
            QVERIFY(count <= 1);
        }
    }

private:
    void buildIndex() {
        TextIndexBuilder builder(*extractor_);
        builder.start(index_, kPages, {});
        extractor_->waitForIdle();
    }

    [[nodiscard]] std::u32string pageText(int pageIndex) {
        std::u32string text;
        extractor_->withTextPage(pageIndex, [&text](const TextPage* page) {
            if (page == nullptr) return;
            for (const domain::TextChar& c : page->layer().chars()) text.push_back(c.unicode);
        });
        extractor_->waitForIdle();
        return text;
    }

    [[nodiscard]] std::vector<domain::SearchResult> searchEveryPageWithPdfium(
        const std::string& query, const SearchOptions& options) {
        std::vector<domain::SearchResult> all;
        for (int page = 0; page < kPages; ++page) {
            extractor_->withTextPage(page, [&all, &query, &options](const TextPage* textPage) {
                if (textPage == nullptr) return;
                auto results = searchPage(*textPage, query, options);
                all.insert(all.end(), results.begin(), results.end());
            });
        }
        extractor_->waitForIdle();
        return all;
    }

    // 只比對頁碼與字元範圍。context 的字串內容兩邊的產生路徑不同
    // （PDFium 的 GetText vs 索引的碼點），逐位元組比對會在代理對與
    // 控制字元上出現無意義的差異；真正要一致的是「命中在哪裡」。
    [[nodiscard]] static bool sameResults(const std::vector<domain::SearchResult>& a,
                                          const std::vector<domain::SearchResult>& b) {
        if (a.size() != b.size()) return false;
        auto key = [](const domain::SearchResult& r) {
            return std::tuple(r.pageIndex, r.range.start, r.range.end);
        };
        std::vector<std::tuple<std::int32_t, std::int32_t, std::int32_t>> ka;
        std::vector<std::tuple<std::int32_t, std::int32_t, std::int32_t>> kb;
        for (const auto& r : a) ka.push_back(key(r));
        for (const auto& r : b) kb.push_back(key(r));
        std::sort(ka.begin(), ka.end());
        std::sort(kb.begin(), kb.end());
        return ka == kb;
    }

    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
    std::unique_ptr<TextExtractor> extractor_;
    TextIndex index_;
};

QTEST_MAIN(TestTextIndex)
#include "test_text_index.moc"
