// 覆蓋頁面（PRD-PAGE-008）與取代頁面（PRD-PAGE-009），WBS 12。
//
// 覆蓋的兩個驗收點：上下層順序、以及三種對齊各自的座標。
// 順序用**渲染**驗——浮水印疊錯層的症狀是「它把正文蓋掉了」或「它完全不見」，
// 兩者在內容串流上都長得很正常，只有畫出來才看得見。
//
// 取代的驗收點是「頁數不變、內容換了、其餘頁不受影響」。第三項最容易破：
// 取代若是靠重建頁面樹達成，很容易連帶動到別頁的 /Parent 或繼承屬性。

#include <QtTest>

#include <QTemporaryDir>

#include <memory>

#include "engine/pageops/page_overlay.h"
#include "pageops_readback.h"
#include "pageops_render.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::pageops;
using alioth::test::pageops::FixtureAnnotation;
using alioth::test::pageops::FixturePage;

namespace {

FixturePage textPage(const std::string& text, const domain::RectF& media = {0, 0, 200, 400}) {
    FixturePage page;
    page.media = media;
    page.text = text;
    page.textAt = domain::PointF{media.left + 20.0, media.bottom + 300.0};
    return page;
}

// 整頁塗滿指定顏色的頁面。上下層順序只有靠顏色分得出來。
FixturePage filledPage(const std::string& text, const char* colourOperator,
                       const domain::RectF& media = {0, 0, 200, 400}) {
    FixturePage page = textPage(text, media);
    page.rawContentPrefix = std::string{colourOperator} + "\n" + "0 0 " +
                            std::to_string(static_cast<int>(media.width())) + " " +
                            std::to_string(static_cast<int>(media.height())) + " re\nf\n";
    return page;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

class TestPageOverlay : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void overlayAboveCoversTheBase() {
        const std::string base =
            test::pageops::makeFixturePdf({filledPage("BASE", "0 0 0 rg")});
        const std::string stamp =
            test::pageops::makeFixturePdf({filledPage("STAMP", "1 0 0 rg")});

        OverlayRequest request;
        request.options.anchor = domain::compose::OverlayAnchor::Stretch;
        request.options.layer = domain::compose::OverlayLayer::Above;

        const OverlayResult result = overlayDocument(base, stamp, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.overlaidPages, 1);
        QCOMPARE(result.pageCount, 1);

        const engine::PixelBufferPtr buffer =
            test::pageops::renderPage(result.bytes, dir_->path(), 0, 200);
        QVERIFY2(buffer != nullptr, "渲染失敗");
        const test::pageops::Pixel pixel = test::pageops::samplePage(
            *buffer, domain::SizeF{200.0, 400.0}, domain::PointF{100.0, 200.0});
        QVERIFY2(pixel.r > 200 && pixel.g < 80,
                 QStringLiteral("疊在上方的印章沒有蓋住底頁：%1,%2,%3")
                     .arg(pixel.r).arg(pixel.g).arg(pixel.b).toUtf8().constData());

        // 兩份文件的文字都還在：疊上去不該讓底頁的內容消失。
        const std::string text = test::pageops::pageText(result.bytes, dir_->path(), 0);
        QVERIFY2(contains(text, "BASE"), text.c_str());
        QVERIFY2(contains(text, "STAMP"), text.c_str());

        assertQpdfClean(result.bytes, QStringLiteral("overlay-above"));
    }

    void overlayBelowStaysUnderTheBase() {
        const std::string base =
            test::pageops::makeFixturePdf({filledPage("BASE", "0 0 0 rg")});
        const std::string watermark =
            test::pageops::makeFixturePdf({filledPage("MARK", "1 0 0 rg")});

        OverlayRequest request;
        request.options.anchor = domain::compose::OverlayAnchor::Stretch;
        request.options.layer = domain::compose::OverlayLayer::Below;

        const OverlayResult result = overlayDocument(base, watermark, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        const engine::PixelBufferPtr buffer =
            test::pageops::renderPage(result.bytes, dir_->path(), 0, 200);
        QVERIFY2(buffer != nullptr, "渲染失敗");
        const test::pageops::Pixel pixel = test::pageops::samplePage(
            *buffer, domain::SizeF{200.0, 400.0}, domain::PointF{100.0, 200.0});
        QVERIFY2(pixel.r < 80 && pixel.g < 80 && pixel.b < 80,
                 QStringLiteral("疊在下方的浮水印蓋掉了正文：%1,%2,%3")
                     .arg(pixel.r).arg(pixel.g).arg(pixel.b).toUtf8().constData());

        // 順序錯了才會蓋住，但浮水印本身必須真的在檔案裡。
        const std::string text = test::pageops::pageText(result.bytes, dir_->path(), 0);
        QVERIFY2(contains(text, "MARK"), text.c_str());

        assertQpdfClean(result.bytes, QStringLiteral("overlay-below"));
    }

    void alignmentPlacesTheStampAtTheExpectedCorner() {
        const std::string base = test::pageops::makeFixturePdf({textPage("BASE")});

        // 100×50 的小印章，文字在它自己的 (10,20)。
        FixturePage stampPage;
        stampPage.media = domain::RectF{0.0, 0.0, 100.0, 50.0};
        stampPage.text = "STAMP";
        stampPage.textAt = domain::PointF{10.0, 20.0};
        stampPage.fontSize = 10.0;
        const std::string stamp = test::pageops::makeFixturePdf({stampPage});

        const struct {
            domain::compose::OverlayAnchor anchor;
            domain::RectF expected;  // 印章在底頁上應該落的範圍
            const char* name;
        } cases[] = {
            {domain::compose::OverlayAnchor::TopLeft, domain::RectF{0, 350, 100, 400}, "左上"},
            {domain::compose::OverlayAnchor::Center, domain::RectF{50, 175, 150, 225}, "置中"},
            {domain::compose::OverlayAnchor::BottomRight, domain::RectF{100, 0, 200, 50}, "右下"},
        };

        for (const auto& item : cases) {
            OverlayRequest request;
            request.options.anchor = item.anchor;

            const OverlayResult result = overlayDocument(base, stamp, request);
            QVERIFY2(result.ok(), result.diagnostic.c_str());

            const std::string inside =
                test::pageops::textInArea(result.bytes, dir_->path(), 0, item.expected);
            QVERIFY2(contains(inside, "STAMP"),
                     (std::string{item.name} + " 對齊沒有把印章放到預期範圍，實際讀到：" + inside)
                         .c_str());

            // 反向確認：印章不該同時出現在對角的那一格，否則上面那條斷言
            // 只是因為範圍取得太寬鬆而通過。
            const domain::RectF opposite{400.0 - item.expected.right, 400.0 - item.expected.top,
                                         400.0 - item.expected.left, 400.0 - item.expected.bottom};
            if (item.anchor != domain::compose::OverlayAnchor::Center) {
                const std::string outside = test::pageops::textInArea(
                    result.bytes, dir_->path(), 0,
                    domain::RectF{opposite.left, opposite.bottom, opposite.left + 100.0,
                                  opposite.bottom + 50.0});
                QVERIFY2(!contains(outside, "STAMP"), outside.c_str());
            }
        }
    }

    void stretchScalesTheOverlayToTheBase() {
        const std::string base = test::pageops::makeFixturePdf({textPage("BASE")});

        FixturePage stampPage;
        stampPage.media = domain::RectF{0.0, 0.0, 100.0, 50.0};
        stampPage.text = "STAMP";
        stampPage.textAt = domain::PointF{10.0, 20.0};
        stampPage.fontSize = 10.0;
        const std::string stamp = test::pageops::makeFixturePdf({stampPage});

        OverlayRequest request;
        request.options.anchor = domain::compose::OverlayAnchor::Stretch;

        const OverlayResult result = overlayDocument(base, stamp, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        // 兩軸分別放大 2 倍與 8 倍，文字落在 (20,160) 附近。
        const std::string inside = test::pageops::textInArea(result.bytes, dir_->path(), 0,
                                                             domain::RectF{0, 120, 200, 240});
        QVERIFY2(contains(inside, "STAMP"), inside.c_str());
    }

    void overlaySurvivesUnbalancedBaseContent() {
        // 底頁少一個 Q 並把填色設成紅色。沒有先用 q/Q 把底頁包起來的話，
        // 疊上去的印章會被染成紅色——而底頁單獨看完全正常。
        FixturePage basePage = textPage("BASE");
        basePage.rawContentPrefix = "q\n1 0 0 rg\n";
        const std::string base = test::pageops::makeFixturePdf({basePage});

        FixturePage stampPage;
        stampPage.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
        stampPage.text = "STAMP";
        stampPage.rawContentPrefix = "40 40 120 120 re\nf\n";  // 不設顏色，預設黑
        const std::string stamp = test::pageops::makeFixturePdf({stampPage});

        OverlayRequest request;
        request.options.anchor = domain::compose::OverlayAnchor::Stretch;
        request.options.layer = domain::compose::OverlayLayer::Above;

        const OverlayResult result = overlayDocument(base, stamp, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        const engine::PixelBufferPtr buffer =
            test::pageops::renderPage(result.bytes, dir_->path(), 0, 400);
        QVERIFY2(buffer != nullptr, "渲染失敗");
        const test::pageops::Pixel pixel = test::pageops::samplePage(
            *buffer, domain::SizeF{200.0, 400.0}, domain::PointF{100.0, 100.0});
        QVERIFY2(pixel.r < 80 && pixel.g < 80 && pixel.b < 80,
                 QStringLiteral("底頁的圖形狀態污染了疊上去的內容：%1,%2,%3")
                     .arg(pixel.r).arg(pixel.g).arg(pixel.b).toUtf8().constData());
    }

    void overlayAnnotationsAreCopiedAndTransformed() {
        const std::string base = test::pageops::makeFixturePdf({textPage("BASE")});

        FixturePage stampPage;
        stampPage.media = domain::RectF{0.0, 0.0, 100.0, 50.0};
        stampPage.text = "STAMP";
        stampPage.textAt = domain::PointF{10.0, 20.0};
        FixtureAnnotation annotation;
        annotation.rect = domain::RectF{10.0, 10.0, 60.0, 30.0};
        stampPage.annotations.push_back(annotation);
        const std::string stamp = test::pageops::makeFixturePdf({stampPage});

        OverlayRequest request;
        request.options.anchor = domain::compose::OverlayAnchor::TopLeft;
        request.copyAnnotations = true;

        const OverlayResult result = overlayDocument(base, stamp, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.copiedAnnotations, 1);

        const std::vector<test::pageops::AnnotationReadback> annotations =
            test::pageops::readAnnotations(result.bytes, 0);
        QCOMPARE(annotations.size(), std::size_t{1});
        // 左上對齊 = 平移 (0, 350)，縮放 1。
        QCOMPARE(annotations[0].rect.left, 10.0);
        QCOMPARE(annotations[0].rect.bottom, 360.0);
        QCOMPARE(annotations[0].rect.top, 380.0);

        assertQpdfClean(result.bytes, QStringLiteral("overlay-annots"));
    }

    // 從檔案插入頁面（PRD-PAGE-001）。
    //
    // 這一條要守的是「插入不是取代」：pageCount 寫成 1 會靜默刪掉插入點那一頁，
    // 而總頁數只差一，使用者很可能好一陣子都不會發現少了一頁。
    void insertAddsPagesWithoutRemovingAny() {
        const std::string base = test::pageops::makeFixturePdf(
            {textPage("ALPHA"), textPage("BRAVO"), textPage("CHARLIE")});
        const std::string source = test::pageops::makeFixturePdf({textPage("ZULU")});

        const ReplaceResult result = insertPagesFrom(base, source, 1);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.removedPages, 0);
        QCOMPARE(result.insertedPages, 1);
        QCOMPARE(result.pageCount, 4);
        QCOMPARE(test::pageops::documentPageCount(result.bytes), 4);

        // 原本的三頁一頁都不能少，而且順序不變。
        const std::string first = test::pageops::pageText(result.bytes, dir_->path(), 0);
        const std::string inserted = test::pageops::pageText(result.bytes, dir_->path(), 1);
        const std::string second = test::pageops::pageText(result.bytes, dir_->path(), 2);
        const std::string third = test::pageops::pageText(result.bytes, dir_->path(), 3);
        QVERIFY2(contains(first, "ALPHA"), first.c_str());
        QVERIFY2(contains(inserted, "ZULU"), inserted.c_str());
        QVERIFY2(contains(second, "BRAVO"), second.c_str());
        QVERIFY2(contains(third, "CHARLIE"), third.c_str());

        assertQpdfClean(result.bytes, QStringLiteral("insert-from-file"));
    }

    void insertAtTheEndAppends() {
        // atIndex == pageCount 是合法的「附加在最後」。越界檢查寫成 >= 會把
        // 這個最常用的情況擋掉。
        const std::string base = test::pageops::makeFixturePdf({textPage("ALPHA")});
        const std::string source = test::pageops::makeFixturePdf({textPage("ZULU")});

        const ReplaceResult result = insertPagesFrom(base, source, 1);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 2);
        const std::string last = test::pageops::pageText(result.bytes, dir_->path(), 1);
        QVERIFY2(contains(last, "ZULU"), last.c_str());
    }

    // 一次插到多個位置（PRD-ANN-028「文件加摘要」）。
    //
    // 這一條要守的是「呼叫端給的是原始編號」：實作若逐一插入而不重算，
    // 每插一頁後面的位置就位移一格，第二個之後的插入點會愈來愈偏。
    // 三個插入點才驗得到累積誤差——兩個的話位移一格剛好還「看起來像對的」。
    void interleaveUsesOriginalPageNumbersForEveryPlacement() {
        const std::string base = test::pageops::makeFixturePdf(
            {textPage("ALPHA"), textPage("BRAVO"), textPage("CHARLIE")});
        const std::string source = test::pageops::makeFixturePdf(
            {textPage("NOTEA"), textPage("NOTEB"), textPage("NOTEC")});

        const ReplaceResult result =
            interleavePagesFrom(base, source, {{0, 0}, {1, 1}, {2, 2}});
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.insertedPages, 3);
        QCOMPARE(result.pageCount, 6);

        const char* expected[] = {"ALPHA", "NOTEA", "BRAVO", "NOTEB", "CHARLIE", "NOTEC"};
        for (int i = 0; i < 6; ++i) {
            const std::string text = test::pageops::pageText(result.bytes, dir_->path(), i);
            QVERIFY2(contains(text, expected[i]),
                     (std::to_string(i) + ": " + text).c_str());
        }
        assertQpdfClean(result.bytes, QStringLiteral("interleave"));
    }

    void interleaveCanInsertBeforeTheFirstPage() {
        const std::string base = test::pageops::makeFixturePdf({textPage("ALPHA")});
        const std::string source = test::pageops::makeFixturePdf({textPage("COVER")});

        const ReplaceResult result = interleavePagesFrom(base, source, {{0, -1}});
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 2);
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 0), "COVER"));
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 1), "ALPHA"));
    }

    void interleaveKeepsMultiplePagesInsertedAtTheSamePointInOrder() {
        // 一頁的摘要長到跨兩頁時，兩頁都掛在同一個插入點上，順序不能顛倒。
        const std::string base = test::pageops::makeFixturePdf({textPage("ALPHA")});
        const std::string source = test::pageops::makeFixturePdf(
            {textPage("FIRST"), textPage("SECOND")});

        const ReplaceResult result = interleavePagesFrom(base, source, {{0, 0}, {1, 0}});
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 3);
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 1), "FIRST"));
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 2), "SECOND"));
    }

    void interleaveRejectsOutOfRangePlacements() {
        const std::string base = test::pageops::makeFixturePdf({textPage("ALPHA")});
        const std::string source = test::pageops::makeFixturePdf({textPage("NOTE")});

        QVERIFY(!interleavePagesFrom(base, source, {{0, 5}}).ok());
        QVERIFY(!interleavePagesFrom(base, source, {{9, 0}}).ok());
        QVERIFY(!interleavePagesFrom(base, source, {{0, -2}}).ok());
        QVERIFY(!interleavePagesFrom(base, source, {}).ok());
    }

    void insertCanTakeASubsetOfTheSourcePages() {
        const std::string base = test::pageops::makeFixturePdf({textPage("ALPHA")});
        const std::string source = test::pageops::makeFixturePdf(
            {textPage("ONE"), textPage("TWO"), textPage("THREE")});

        const ReplaceResult result = insertPagesFrom(base, source, 1, {2});
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 2);
        const std::string inserted = test::pageops::pageText(result.bytes, dir_->path(), 1);
        QVERIFY2(contains(inserted, "THREE"), inserted.c_str());
        QVERIFY2(!contains(inserted, "ONE"), inserted.c_str());
    }

    void insertRejectsAnOutOfRangePosition() {
        const std::string base = test::pageops::makeFixturePdf({textPage("ALPHA")});
        const std::string source = test::pageops::makeFixturePdf({textPage("ZULU")});
        QVERIFY(!insertPagesFrom(base, source, 5).ok());
        QVERIFY(!insertPagesFrom(base, source, -1).ok());
    }

    void replaceKeepsPageCountAndLeavesOtherPagesAlone() {
        const std::string base = test::pageops::makeFixturePdf(
            {textPage("ALPHA"), textPage("BRAVO"), textPage("CHARLIE")});
        const std::string replacement = test::pageops::makeFixturePdf({textPage("ZULU")});

        ReplaceRequest request;
        request.firstPage = 1;
        request.pageCount = 1;

        const ReplaceResult result = replacePages(base, replacement, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 3);
        QCOMPARE(test::pageops::documentPageCount(result.bytes), 3);

        // 內容換了。
        const std::string replaced = test::pageops::pageText(result.bytes, dir_->path(), 1);
        QVERIFY2(contains(replaced, "ZULU"), replaced.c_str());
        QVERIFY2(!contains(replaced, "BRAVO"), replaced.c_str());

        // 其餘頁不受影響——這一項最容易破，因為取代要重建頁面樹。
        const std::string first = test::pageops::pageText(result.bytes, dir_->path(), 0);
        const std::string third = test::pageops::pageText(result.bytes, dir_->path(), 2);
        QVERIFY2(contains(first, "ALPHA"), first.c_str());
        QVERIFY2(contains(third, "CHARLIE"), third.c_str());

        assertQpdfClean(result.bytes, QStringLiteral("replace-single"));
    }

    void replaceBringsTheReplacementAnnotations() {
        const std::string base =
            test::pageops::makeFixturePdf({textPage("ALPHA"), textPage("BRAVO")});

        FixturePage source = textPage("ZULU");
        FixtureAnnotation annotation;
        annotation.rect = domain::RectF{30.0, 200.0, 130.0, 220.0};
        source.annotations.push_back(annotation);
        const std::string replacement = test::pageops::makeFixturePdf({source});

        ReplaceRequest request;
        request.firstPage = 0;
        request.pageCount = 1;

        const ReplaceResult result = replacePages(base, replacement, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        const std::vector<test::pageops::AnnotationReadback> annotations =
            test::pageops::readAnnotations(result.bytes, 0);
        QCOMPARE(annotations.size(), std::size_t{1});
        // 取代不做任何幾何變換，座標必須原封不動。
        QCOMPARE(annotations[0].rect.left, 30.0);
        QCOMPARE(annotations[0].rect.top, 220.0);
    }

    void replaceCanSubstituteManyPagesForOne() {
        const std::string base = test::pageops::makeFixturePdf(
            {textPage("ALPHA"), textPage("BRAVO"), textPage("CHARLIE")});
        const std::string replacement =
            test::pageops::makeFixturePdf({textPage("YANKEE"), textPage("ZULU")});

        ReplaceRequest request;
        request.firstPage = 1;
        request.pageCount = 1;  // 一頁換兩頁

        const ReplaceResult result = replacePages(base, replacement, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 4);
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 1), "YANKEE"));
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 2), "ZULU"));
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 3), "CHARLIE"));
    }

    void outOfRangeRequestsAreRejected() {
        const std::string base =
            test::pageops::makeFixturePdf({textPage("ALPHA"), textPage("BRAVO")});
        const std::string replacement = test::pageops::makeFixturePdf({textPage("ZULU")});

        ReplaceRequest tooFar;
        tooFar.firstPage = 2;
        tooFar.pageCount = 1;
        QCOMPARE(replacePages(base, replacement, tooFar).status, PageOpsStatus::PageOutOfRange);

        ReplaceRequest badSource;
        badSource.firstPage = 0;
        badSource.pageCount = 1;
        badSource.replacementPages = {5};
        QCOMPARE(replacePages(base, replacement, badSource).status, PageOpsStatus::PageOutOfRange);

        OverlayRequest overlay;
        overlay.overlayPageIndex = 3;
        QCOMPARE(overlayDocument(base, replacement, overlay).status,
                 PageOpsStatus::PageOutOfRange);
    }

private:
    void assertQpdfClean(const std::string& bytes, const QString& name) {
        const QString path = dir_->path() + QStringLiteral("/%1.pdf").arg(name);
        QVERIFY(test::pageops::writeBytes(path, bytes));
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        if (check.status == test::QpdfStatus::NotAvailable) QSKIP("找不到 qpdf，結構檢查略過");
        QVERIFY2(check.clean(), check.output.toUtf8().constData());
    }

    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestPageOverlay)
#include "test_page_overlay.moc"
