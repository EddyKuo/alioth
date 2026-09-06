// Bates 編號與頁面範圍解析（PRD-PAGE-004、PRD-IO-008）。
//
// 這兩件事是列印裡唯二「錯了也不會當掉、但輸出直接失去價值」的邏輯：
// 跳號的 Bates 讓卷宗無法交叉引用，錯誤的範圍解析讓使用者少印卻不自知。
// 因此它們被刻意做成純函式，這支測試逐一列舉邊界。

#include <QtTest>

#include "app/print/bates_numbering.h"
#include "app/print/page_range.h"
#include "app/print/stamp_content_stream.h"
#include "app/print/stamp_layout.h"

using namespace alioth::app::print;

class TestBatesNumbering : public QObject {
    Q_OBJECT

private slots:
    void formatsPrefixSuffixAndPadding() {
        BatesOptions options;
        options.enabled = true;
        options.prefix = QStringLiteral("ABC-");
        options.suffix = QStringLiteral("-EX");
        options.startNumber = 42;
        options.digits = 6;

        QCOMPARE(formatBatesNumber(options, 0), QStringLiteral("ABC-000042-EX"));
        QCOMPARE(formatBatesNumber(options, 1), QStringLiteral("ABC-000043-EX"));
    }

    void zeroDigitsMeansNoPadding() {
        BatesOptions options;
        options.startNumber = 7;
        options.digits = 0;
        QCOMPARE(formatBatesNumber(options, 0), QStringLiteral("7"));
    }

    void paddingNeverTruncatesLongerNumbers() {
        // 位數只是最少寬度。號碼超過位數時截斷會產生重號，那比字串變長嚴重得多。
        BatesOptions options;
        options.startNumber = 1234567;
        options.digits = 3;
        QCOMPARE(formatBatesNumber(options, 0), QStringLiteral("1234567"));
    }

    void digitsAreClampedToSaneMaximum() {
        BatesOptions options;
        options.startNumber = 1;
        options.digits = 999;  // 多半是把起始號填進了位數欄
        QCOMPARE(formatBatesNumber(options, 0).size(), kMaxBatesDigits);
    }

    void incrementAppliesPerPage() {
        BatesOptions options;
        options.startNumber = 100;
        options.increment = 5;
        options.digits = 4;
        QCOMPARE(formatBatesNumber(options, 0), QStringLiteral("0100"));
        QCOMPARE(formatBatesNumber(options, 3), QStringLiteral("0115"));
    }

    void negativeValuesKeepSignOutsidePadding() {
        BatesOptions options;
        options.startNumber = 2;
        options.increment = -1;
        options.digits = 3;
        QCOMPARE(formatBatesNumber(options, 4), QStringLiteral("-002"));
    }

    void hugeOrdinalDoesNotOverflowIntoWrongNumber() {
        BatesOptions options;
        options.startNumber = 1;
        options.increment = 1'000'000'000;
        options.digits = 0;
        // 溢位後回傳起始號是保守行為；重點是不得回傳一個看起來合理的錯號碼。
        QCOMPARE(batesValueAt(options, 9'000'000'000LL), 1LL);
    }

    void printSequenceBasisNumbersOnlyPrintedPages() {
        BatesOptions options;
        options.enabled = true;
        options.digits = 4;
        options.basis = BatesBasis::PrintSequence;

        // 只印第 5、6、7 頁（0-based 4、5、6）時，本批自成一份連續卷宗。
        const std::vector<QString> numbers = batesSequence(options, {4, 5, 6});
        QCOMPARE(numbers.size(), std::size_t{3});
        QCOMPARE(numbers[0], QStringLiteral("0001"));
        QCOMPARE(numbers[1], QStringLiteral("0002"));
        QCOMPARE(numbers[2], QStringLiteral("0003"));
    }

    void documentPageBasisKeepsNumbersAlignedWithFullDocument() {
        BatesOptions options;
        options.enabled = true;
        options.digits = 4;
        options.basis = BatesBasis::DocumentPage;

        const std::vector<QString> numbers = batesSequence(options, {4, 5, 6});
        QCOMPARE(numbers[0], QStringLiteral("0005"));
        QCOMPARE(numbers[2], QStringLiteral("0007"));
    }

    void reverseOrderKeepsDocumentPageNumbersWithTheirPages() {
        // 反序列印時，DocumentPage 基準的號碼必須跟著頁面走而不是跟著紙張走。
        BatesOptions options;
        options.enabled = true;
        options.digits = 3;
        options.basis = BatesBasis::DocumentPage;

        const std::vector<QString> numbers = batesSequence(options, {2, 1, 0});
        QCOMPARE(numbers[0], QStringLiteral("003"));
        QCOMPARE(numbers[2], QStringLiteral("001"));
    }

    void zeroIncrementIsDetectedAsDuplicateSequence() {
        BatesOptions options;
        options.enabled = true;
        options.increment = 0;
        const std::vector<QString> numbers = batesSequence(options, {0, 1, 2});
        QVERIFY(!isBatesSequenceUnique(numbers));
    }

    void normalSequenceIsUnique() {
        BatesOptions options;
        options.enabled = true;
        QVERIFY(isBatesSequenceUnique(batesSequence(options, {0, 1, 2, 3, 4})));
    }

    // ---- 頁面範圍 ----

    void emptySpecMeansAllPages() {
        const PageRangeResult result = parsePageRange(QString(), 4);
        QVERIFY(result.valid);
        QCOMPARE(result.pages, std::vector<int>({0, 1, 2, 3}));
    }

    void parsesListsAndRanges() {
        const PageRangeResult result = parsePageRange(QStringLiteral("1-3,5"), 10);
        QVERIFY(result.valid);
        QCOMPARE(result.pages, std::vector<int>({0, 1, 2, 4}));
    }

    void openEndedRangesUseDocumentBounds() {
        QCOMPARE(parsePageRange(QStringLiteral("-3"), 10).pages, std::vector<int>({0, 1, 2}));
        QCOMPARE(parsePageRange(QStringLiteral("8-"), 10).pages, std::vector<int>({7, 8, 9}));
    }

    void descendingRangeKeepsWrittenOrder() {
        QCOMPARE(parsePageRange(QStringLiteral("3-1"), 10).pages, std::vector<int>({2, 1, 0}));
    }

    void duplicatesAreRemoved() {
        QCOMPARE(parsePageRange(QStringLiteral("1,1,2,1-2"), 10).pages, std::vector<int>({0, 1}));
    }

    void outOfBoundsIsClampedNotRejected() {
        const PageRangeResult result = parsePageRange(QStringLiteral("1-9999"), 3);
        QVERIFY(result.valid);
        QCOMPARE(result.pages.size(), std::size_t{3});
    }

    void malformedInputFailsLoudly() {
        QVERIFY(!parsePageRange(QStringLiteral("abc"), 10).valid);
        QVERIFY(!parsePageRange(QStringLiteral("2-x"), 10).valid);
        QVERIFY(!parsePageRange(QStringLiteral("-"), 10).valid);
        QVERIFY(!parsePageRange(QStringLiteral("1"), 0).valid);
    }

    void oddAndEvenSubsets() {
        QCOMPARE(parsePageRange(QString(), 6, PageSubset::Odd).pages, std::vector<int>({0, 2, 4}));
        QCOMPARE(parsePageRange(QString(), 6, PageSubset::Even).pages, std::vector<int>({1, 3, 5}));
    }

    void emptySelectionIsReportedInsteadOfPrintingNothing() {
        const PageRangeResult result = parsePageRange(QStringLiteral("2,4"), 10, PageSubset::Odd);
        QVERIFY(!result.valid);
        QVERIFY(!result.diagnostic.isEmpty());
    }

    void reverseHelperFlipsOrder() {
        QCOMPARE(reversed({0, 1, 2}), std::vector<int>({2, 1, 0}));
    }

    // ---- 九宮格與符號 ----

    void nineCellPlacement() {
        const QRectF box{0.0, 0.0, 100.0, 100.0};
        const QSizeF item{20.0, 10.0};

        QCOMPARE(placeStamp(box, item, StampAnchor::TopLeft).topLeft(), QPointF(0.0, 0.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::TopCenter).topLeft(), QPointF(40.0, 0.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::TopRight).topLeft(), QPointF(80.0, 0.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::MiddleLeft).topLeft(), QPointF(0.0, 45.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::Center).topLeft(), QPointF(40.0, 45.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::MiddleRight).topLeft(), QPointF(80.0, 45.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::BottomLeft).topLeft(), QPointF(0.0, 90.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::BottomCenter).topLeft(), QPointF(40.0, 90.0));
        QCOMPARE(placeStamp(box, item, StampAnchor::BottomRight).topLeft(), QPointF(80.0, 90.0));
    }

    void marginsShrinkTheContentBox() {
        const QRectF printable{0.0, 0.0, 595.0, 842.0};
        StampMargins margins;
        margins.left = 10.0;
        margins.top = 20.0;
        margins.right = 30.0;
        margins.bottom = 40.0;
        const QRectF box = stampContentBox(printable, margins);
        QCOMPARE(box.left(), 10.0);
        QCOMPARE(box.top(), 20.0);
        QCOMPARE(box.right(), 565.0);
        QCOMPARE(box.bottom(), 802.0);
    }

    void oversizedMarginsYieldEmptyBoxRatherThanInvertedRect() {
        StampMargins margins;
        margins.left = 400.0;
        margins.right = 400.0;
        QVERIFY(stampContentBox(QRectF{0.0, 0.0, 595.0, 842.0}, margins).isEmpty());
    }

    void tokenExpansion() {
        StampContext context;
        context.pageNumber = 7;
        context.pageCount = 12;
        context.printSequence = 2;
        context.sheetNumber = 3;
        context.batesText = QStringLiteral("ABC-000042");
        context.fileName = QStringLiteral("case.pdf");
        context.timestamp = QDateTime(QDate(2026, 9, 5), QTime(13, 5, 9));

        const QString out = expandStampTokens(
            QStringLiteral("<<FileName>> <<Page>>/<<Pages>> seq<<Sequence>> sheet<<Sheet>> "
                           "<<Bates>> <<Date>> <<Time>>"),
            context);
        QCOMPARE(out, QStringLiteral("case.pdf 7/12 seq2 sheet3 ABC-000042 2026-09-05 13:05:09"));
    }

    // ---- 內容串流（把戳記寫進文件的路徑） ----

    void streamNumbersAreLocaleIndependentAndTrimmed() {
        QCOMPARE(QString::fromStdString(formatStreamNumber(12.0)), QStringLiteral("12"));
        QCOMPARE(QString::fromStdString(formatStreamNumber(12.5)), QStringLiteral("12.5"));
        QCOMPARE(QString::fromStdString(formatStreamNumber(-0.25)), QStringLiteral("-0.25"));
        QCOMPARE(QString::fromStdString(formatStreamNumber(0.0)), QStringLiteral("0"));
    }

    void pdfStringsEscapeParenthesesAndBackslashes() {
        QCOMPARE(QString::fromStdString(escapePdfLiteralString("A(B)\\C")),
                 QStringLiteral("A\\(B\\)\\\\C"));
    }

    void textStampStreamIsWellFormed() {
        StampStreamOptions options;
        options.fontSize = 10.0;
        const StampStream stream = makeTextStampStream("ABC-000042", 100.0, 36.0, options);
        QVERIFY(stream.valid);
        const QString content = QString::fromStdString(stream.content);
        QVERIFY(content.startsWith(QStringLiteral("q\n")));
        QVERIFY(content.contains(QStringLiteral("/F0 10 Tf")));
        QVERIFY(content.contains(QStringLiteral("1 0 0 1 100 36 Tm")));
        QVERIFY(content.contains(QStringLiteral("(ABC-000042) Tj")));
        QVERIFY(content.endsWith(QStringLiteral("Q\n")));
    }

    void cjkStampSplitsRunsAndReportsTheCodepointsToEmbed() {
        // ADR-007：CJK 走內嵌子集。拉丁與 CJK 必須分段畫——同一段混用的話，
        // 不是中文變亂碼就是拉丁字被當成雙位元組讀掉，兩種都是「畫出來是
        // 別的東西」而不是明顯的失敗。
        StampStreamOptions options;
        options.fontResourceName = "F0";
        options.cjkFontResourceName = "CJK";
        options.fontSize = 10.0;

        const StampStream stream = makeTextStampStream("\xE6\xA1\x88-0001", 0.0, 0.0, options);
        QVERIFY2(stream.valid, stream.diagnostic.c_str());

        const QString content = QString::fromStdString(stream.content);
        QVERIFY2(content.contains(QStringLiteral("/CJK 10 Tf")), "CJK 段沒有切換到內嵌字型");
        QVERIFY2(content.contains(QStringLiteral("/F0 10 Tf")), "拉丁段沒有切回標準字型");

        // 呼叫端要靠這份碼點清單去子集化並內嵌。回報漏了的話，畫面上是空白——
        // 而空白的戳記看起來像功能沒作用。
        QVERIFY(stream.cjkCodepoints.count(U'案') == 1);
        QVERIFY(stream.cjkCodepoints.count(U'-') == 0);
    }

    void cjkStampWithoutAFontResourceNameFailsLoudly() {
        StampStreamOptions options;
        options.cjkFontResourceName.clear();
        const StampStream stream = makeTextStampStream("\xE6\xA1\x88-0001", 0.0, 0.0, options);
        QVERIFY(!stream.valid);
        QVERIFY(!stream.diagnostic.empty());
    }

    void baselineOriginUsesPdfBottomLeftOrigin() {
        StampMargins margins;
        margins.left = 36.0;
        margins.right = 36.0;
        margins.top = 36.0;
        margins.bottom = 36.0;

        const alioth::domain::PointF bottomRight = stampBaselineOrigin(
            612.0, 792.0, 100.0, 12.0, StampAnchor::BottomRight, margins);
        QCOMPARE(bottomRight.x, 612.0 - 36.0 - 100.0);
        QCOMPARE(bottomRight.y, 36.0);

        const alioth::domain::PointF topLeft =
            stampBaselineOrigin(612.0, 792.0, 100.0, 12.0, StampAnchor::TopLeft, margins);
        QCOMPARE(topLeft.x, 36.0);
        QCOMPARE(topLeft.y, 792.0 - 36.0 - 12.0);
    }
};

QTEST_APPLESS_MAIN(TestBatesNumbering)
#include "test_bates_numbering.moc"
