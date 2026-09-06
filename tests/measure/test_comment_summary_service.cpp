// 註解摘要服務測試（PRD-ANN-028）。
//
// 涵蓋「僅摘要」的排序與輸出，以及「文件加摘要」要用到的兩件事：
// 逐頁的摘要文字，以及哪幾頁有註解。真正的頁面插入在
// tests/pageops/test_page_overlay.cpp（interleavePagesFrom）。

#include <QtTest>

#include "app/comment_summary_service.h"

using namespace alioth::app;
using alioth::domain::AnnotationSummary;

namespace {

AnnotationSummary makeSummary(int page, int indexOnPage, std::string subtype, std::string author,
                              std::string contents) {
    AnnotationSummary summary{};
    summary.pageIndex = page;
    summary.indexOnPage = indexOnPage;
    summary.subtype = std::move(subtype);
    summary.author = std::move(author);
    summary.contents = std::move(contents);
    return summary;
}

}  // namespace

class TestCommentSummaryService : public QObject {
    Q_OBJECT

private slots:
    void buildSortsByPageThenPositionRegardlessOfInputOrder() {
        std::vector<AnnotationSummary> annotations = {
            makeSummary(2, 0, "Highlight", "Alice", "第三頁的東西"),
            makeSummary(0, 1, "Note", "Bob", "第一頁第二則"),
            makeSummary(0, 0, "Highlight", "Alice", "第一頁第一則"),
        };

        const std::vector<CommentSummaryEntry> entries = CommentSummaryService::build(annotations);
        QCOMPARE(static_cast<int>(entries.size()), 3);
        QCOMPARE(entries[0].source.pageIndex, 0);
        QCOMPARE(entries[0].source.indexOnPage, 0);
        QCOMPARE(entries[1].source.pageIndex, 0);
        QCOMPARE(entries[1].source.indexOnPage, 1);
        QCOMPARE(entries[2].source.pageIndex, 2);
    }

    void displayTextContainsPageAuthorAndContents() {
        const std::vector<AnnotationSummary> annotations = {
            makeSummary(4, 0, "Highlight", "審閱者", "這段要再確認"),
        };
        const std::vector<CommentSummaryEntry> entries = CommentSummaryService::build(annotations);
        QCOMPARE(static_cast<int>(entries.size()), 1);
        const QString& text = entries.front().displayText;
        QVERIFY(text.contains(QStringLiteral("p.5")));  // 顯示給使用者的頁碼從 1 起算
        QVERIFY(text.contains(QStringLiteral("Highlight")));
        QVERIFY(text.contains(QStringLiteral("審閱者")));
        QVERIFY(text.contains(QStringLiteral("這段要再確認")));
    }

    void renderSummaryOnlyTextProducesOneLinePerEntry() {
        const std::vector<AnnotationSummary> annotations = {
            makeSummary(0, 0, "Highlight", "A", "one"),
            makeSummary(0, 1, "Note", "B", "two"),
        };
        const std::vector<CommentSummaryEntry> entries = CommentSummaryService::build(annotations);
        const QString rendered = CommentSummaryService::renderSummaryOnlyText(entries);
        const QStringList lines = rendered.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 2);
    }

    void pageSummaryOnlyCoversThatPage() {
        const std::vector<AnnotationSummary> annotations = {
            makeSummary(0, 0, "Highlight", "A", "first page note"),
            makeSummary(2, 0, "Note", "B", "third page note"),
            makeSummary(2, 1, "Square", "C", "third page again"),
        };
        const std::vector<CommentSummaryEntry> entries = CommentSummaryService::build(annotations);

        const QString page2 = CommentSummaryService::renderPageSummaryText(entries, 2);
        QVERIFY(page2.contains(QStringLiteral("third page note")));
        QVERIFY(page2.contains(QStringLiteral("third page again")));
        // 別頁的內容混進來，使用者會在第 3 頁的摘要裡讀到第 1 頁的意見。
        QVERIFY(!page2.contains(QStringLiteral("first page note")));
        QVERIFY(page2.contains(QStringLiteral("(2)")));  // 該頁的則數
    }

    void pageWithoutCommentsProducesNothing() {
        // 空字串是「不要插頁」的訊號。回一個只有標題的頁面，會讓「文件加摘要」
        // 在沒有註解的頁後面插一張看起來像出錯的空白頁。
        const std::vector<CommentSummaryEntry> entries =
            CommentSummaryService::build({makeSummary(0, 0, "Note", "A", "x")});
        QVERIFY(CommentSummaryService::renderPageSummaryText(entries, 5).isEmpty());
    }

    void pagesWithCommentsAreSortedAndDeduplicated() {
        const std::vector<AnnotationSummary> annotations = {
            makeSummary(4, 0, "Note", "A", "x"),
            makeSummary(1, 0, "Note", "A", "y"),
            makeSummary(4, 1, "Note", "A", "z"),
            makeSummary(1, 1, "Note", "A", "w"),
        };
        const std::vector<CommentSummaryEntry> entries = CommentSummaryService::build(annotations);
        const std::vector<int> pages = CommentSummaryService::pagesWithComments(entries);
        QCOMPARE(pages, (std::vector<int>{1, 4}));
    }

    void emptyInputProducesEmptyOutput() {
        const std::vector<CommentSummaryEntry> entries = CommentSummaryService::build({});
        QVERIFY(entries.empty());
        QVERIFY(CommentSummaryService::renderSummaryOnlyText(entries).isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestCommentSummaryService)
#include "test_comment_summary_service.moc"
