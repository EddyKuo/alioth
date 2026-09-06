// 頁面對齊（PRD-CMP-001）。
//
// 這是文件比對裡最容易錯、也最容易被忽略的一段：在中間插入一頁之後，
// 若對齊還假設頁碼一一對應，後面每一頁都會被報成「整頁改寫」。
// 使用者看到滿江紅，然後就不再相信這個功能。

#include <QtTest>

#include <string>
#include <vector>

#include "engine/compare/duplicate_pages.h"
#include "engine/compare/page_align.h"
#include "engine/compare/page_tokens.h"

using namespace alioth;
using namespace alioth::engine::compare;

namespace {

std::vector<PageTokens> pagesFrom(TokenTable& table, const std::vector<std::string>& texts) {
    std::vector<PageTokens> pages;
    pages.reserve(texts.size());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        pages.push_back(tokenizeText(static_cast<std::int32_t>(i), texts[i], table));
    }
    return pages;
}

// 找出某一頁在對齊結果中的配對。
const domain::PageAlignment* alignmentForOldPage(const AlignResult& result, std::int32_t oldPage) {
    for (const domain::PageAlignment& a : result.alignments) {
        if (a.oldPage == oldPage) return &a;
    }
    return nullptr;
}

const domain::PageAlignment* alignmentForNewPage(const AlignResult& result, std::int32_t newPage) {
    for (const domain::PageAlignment& a : result.alignments) {
        if (a.newPage == newPage) return &a;
    }
    return nullptr;
}

}  // namespace

class TestPageAlign : public QObject {
    Q_OBJECT

private slots:
    void identicalDocumentsAlignOneToOne() {
        TokenTable table;
        const std::vector<std::string> texts{"first page content", "second page content",
                                             "third page content"};
        const auto oldPages = pagesFrom(table, texts);
        const auto newPages = pagesFrom(table, texts);

        const AlignResult result = alignPages(oldPages, newPages);
        QVERIFY(!result.rejected);
        QCOMPARE(result.alignments.size(), std::size_t{3});
        for (std::int32_t i = 0; i < 3; ++i) {
            const domain::PageAlignment* a = alignmentForOldPage(result, i);
            QVERIFY(a != nullptr);
            QCOMPARE(a->kind, domain::PageMatchKind::Matched);
            QCOMPARE(a->newPage, i);
            QVERIFY(a->similarity > 0.99);
        }
    }

    void insertedPageDoesNotShiftEverythingAfterIt() {
        // 本檔存在的主要理由。插入一頁之後，舊的第 1、2 頁必須仍然對到
        // 新的第 2、3 頁，而不是被報成「內容全變了」。
        TokenTable table;
        const auto oldPages = pagesFrom(table, {"alpha content here", "beta content here",
                                                "gamma content here"});
        const auto newPages = pagesFrom(table, {"alpha content here", "brand new inserted page",
                                                "beta content here", "gamma content here"});

        const AlignResult result = alignPages(oldPages, newPages);
        QVERIFY(!result.rejected);

        const domain::PageAlignment* beta = alignmentForOldPage(result, 1);
        QVERIFY(beta != nullptr);
        QCOMPARE(beta->kind, domain::PageMatchKind::Matched);
        QCOMPARE(beta->newPage, 2);

        const domain::PageAlignment* gamma = alignmentForOldPage(result, 2);
        QVERIFY(gamma != nullptr);
        QCOMPARE(gamma->newPage, 3);

        const domain::PageAlignment* inserted = alignmentForNewPage(result, 1);
        QVERIFY(inserted != nullptr);
        QCOMPARE(inserted->kind, domain::PageMatchKind::Inserted);
        QCOMPARE(inserted->oldPage, domain::kNoPage);
    }

    void deletedPageIsReportedAsDeleted() {
        TokenTable table;
        const auto oldPages = pagesFrom(table, {"alpha content here", "beta content here",
                                                "gamma content here"});
        const auto newPages = pagesFrom(table, {"alpha content here", "gamma content here"});

        const AlignResult result = alignPages(oldPages, newPages);
        const domain::PageAlignment* beta = alignmentForOldPage(result, 1);
        QVERIFY(beta != nullptr);
        QCOMPARE(beta->kind, domain::PageMatchKind::Deleted);
        QCOMPARE(beta->newPage, domain::kNoPage);

        const domain::PageAlignment* gamma = alignmentForOldPage(result, 2);
        QVERIFY(gamma != nullptr);
        QCOMPARE(gamma->kind, domain::PageMatchKind::Matched);
        QCOMPARE(gamma->newPage, 1);
    }

    void editedPageStillMatchesWhenMostContentSurvives() {
        // 改幾個詞的頁面應該配成 Matched 而不是「刪一頁加一頁」，
        // 否則使用者看不到頁內的差異，只看到整頁換掉。
        TokenTable table;
        const auto oldPages = pagesFrom(
            table, {"the quick brown fox jumps over the lazy dog near the river bank"});
        const auto newPages = pagesFrom(
            table, {"the quick brown cat jumps over the lazy dog near the river bank"});

        const AlignResult result = alignPages(oldPages, newPages);
        QCOMPARE(result.alignments.size(), std::size_t{1});
        QCOMPARE(result.alignments.front().kind, domain::PageMatchKind::Matched);
        QVERIFY(result.alignments.front().similarity > 0.8);
        QVERIFY(result.alignments.front().similarity < 1.0);
    }

    void completelyRewrittenPageIsNotForcedToMatch() {
        // 相似度低於門檻時，「刪一頁、加一頁」比「這頁被改成那樣」好讀。
        TokenTable table;
        const auto oldPages = pagesFrom(table, {"alpha beta gamma delta epsilon"});
        const auto newPages = pagesFrom(table, {"one two three four five six"});

        const AlignResult result = alignPages(oldPages, newPages);
        for (const domain::PageAlignment& a : result.alignments) {
            QVERIFY(a.kind != domain::PageMatchKind::Matched);
        }
    }

    void reorderedPagesAreMatchedNotRewritten() {
        TokenTable table;
        const auto oldPages =
            pagesFrom(table, {"alpha content here", "beta content here", "gamma content here"});
        const auto newPages =
            pagesFrom(table, {"gamma content here", "alpha content here", "beta content here"});

        const AlignResult result = alignPages(oldPages, newPages);
        // 不論實作是否支援跨位置配對，都不得把三頁全部報成刪除加新增：
        // 那等於說「整份文件都變了」，而實際上一個字都沒改。
        int matched = 0;
        for (const domain::PageAlignment& a : result.alignments) {
            if (a.kind == domain::PageMatchKind::Matched) ++matched;
        }
        QVERIFY2(matched >= 1, "重排頁面被整份報成刪除與新增");
    }

    void emptyDocumentsOnEitherSide() {
        TokenTable table;
        const auto pages = pagesFrom(table, {"only content"});

        const AlignResult allInserted = alignPages({}, pages);
        QCOMPARE(allInserted.alignments.size(), std::size_t{1});
        QCOMPARE(allInserted.alignments.front().kind, domain::PageMatchKind::Inserted);

        const AlignResult allDeleted = alignPages(pages, {});
        QCOMPARE(allDeleted.alignments.size(), std::size_t{1});
        QCOMPARE(allDeleted.alignments.front().kind, domain::PageMatchKind::Deleted);

        const AlignResult bothEmpty = alignPages({}, {});
        QVERIFY(bothEmpty.alignments.empty());
        QVERIFY(!bothEmpty.rejected);
    }

    void tooManyPagesIsRejected() {
        TokenTable table;
        AlignOptions options;
        options.maxPages = 2;
        const auto pages = pagesFrom(table, {"a", "b", "c"});

        const AlignResult result = alignPages(pages, pages, options);
        QVERIFY(result.rejected);
    }

    void similarityOfEmptyPagesIsOne() {
        // 掃描件整份都沒有文字層。回 0 會讓它們互相排斥，
        // 結果是每一頁都被報成「刪一頁加一頁」。
        TokenTable table;
        const PageTokens a = tokenizeText(0, "", table);
        const PageTokens b = tokenizeText(1, "", table);
        QCOMPARE(pageSimilarity(a, b), 1.0);
    }

    void duplicatePagesAreGroupedByTextAndSize() {
        std::vector<PageFingerprintInput> pages;
        const auto add = [&pages](std::int32_t index, const char* text, double w, double h) {
            PageFingerprintInput input;
            input.pageIndex = index;
            input.text = text;
            input.sizePt = domain::SizeF{w, h};
            pages.push_back(input);
        };

        add(0, "identical content", 595.0, 842.0);
        add(1, "identical content", 595.0, 842.0);
        // 文字相同但尺寸不同：A4 的簽名頁與 A3 的放大版不是重複頁。
        add(2, "identical content", 842.0, 1191.0);
        add(3, "different content", 595.0, 842.0);

        const auto groups = findDuplicatePages(pages);
        QCOMPARE(groups.size(), std::size_t{1});
        QCOMPARE(groups.front().count(), std::size_t{2});
        QVERIFY(groups.front().pages[0] == 0 && groups.front().pages[1] == 1);
    }

    void fingerprintNormalisationIgnoresWhitespaceOnly() {
        // 換行與多重空白是排版的產物，不是內容差異。
        QCOMPARE(normalizeForFingerprint("hello   world"), normalizeForFingerprint("hello world"));
        QCOMPARE(normalizeForFingerprint(" hello world "), normalizeForFingerprint("hello world"));
        QVERIFY(normalizeForFingerprint("hello world") != normalizeForFingerprint("hello worlds"));
    }
};

QTEST_APPLESS_MAIN(TestPageAlign)
#include "test_page_align.moc"
