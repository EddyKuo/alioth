// 合併頁面：多頁疊為一頁（PRD-PAGE-007，WBS 12）。
//
// 三件事必須成立，缺一項這個功能就不能用：
//
//   一、每個來源頁都出現在輸出，而且在版面指定的格子裡（用文字層讀回來驗，
//       因為那是「使用者看到的東西在哪裡」唯一可信的答案）
//   二、註解跟著搬到對應的位置——漏掉的話頁面看起來完全正確，螢光筆卻在原位
//   三、來源頁之間的圖形狀態不互相污染。這一項用**渲染**驗而不是看串流，
//       因為「串流長得對」與「畫出來是對的」之間的距離正是本項的全部風險

#include <QtTest>

#include <QTemporaryDir>

#include <atomic>
#include <memory>

#include "engine/pageops/page_merge.h"
#include "pageops_readback.h"
#include "pageops_render.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::pageops;
using alioth::test::pageops::FixtureAnnotation;
using alioth::test::pageops::FixturePage;

namespace {

FixturePage textPage(const std::string& text) {
    FixturePage page;
    page.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
    page.text = text;
    page.textAt = domain::PointF{20.0, 300.0};
    return page;
}

// 一份四頁、每頁一段可辨識文字的語料。
std::vector<FixturePage> fourPages() {
    return {textPage("ALPHA"), textPage("BRAVO"), textPage("CHARLIE"), textPage("DELTA")};
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

class TestPageMerge : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void twoUpPlacesBothSourcesInTheirCells() {
        const std::string source =
            test::pageops::makeFixturePdf({textPage("ALPHA"), textPage("BRAVO")});

        // 前提檢查：合併之前兩段文字各自讀得到。少了這一步，「讀得到」
        // 有可能只是因為語料本來就抽不出文字，測試會在沒驗到東西時變綠。
        QVERIFY(contains(test::pageops::pageText(source, dir_->path(), 0), "ALPHA"));

        MergePagesRequest request;
        request.pages = {0, 1};
        request.layout = domain::compose::MergeLayout::nUp(2);
        request.layout.pageSize = domain::SizeF{200.0, 200.0};  // 縮到一半，順便驗縮放

        const MergePagesResult result = mergePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 1);

        const std::string text = test::pageops::pageText(result.bytes, dir_->path(), 0);
        QVERIFY2(contains(text, "ALPHA"), text.c_str());
        QVERIFY2(contains(text, "BRAVO"), text.c_str());

        // 位置：左半格只該有 ALPHA，右半格只該有 BRAVO。
        const std::string left =
            test::pageops::textInArea(result.bytes, dir_->path(), 0, domain::RectF{0, 0, 100, 200});
        const std::string right = test::pageops::textInArea(result.bytes, dir_->path(), 0,
                                                            domain::RectF{100, 0, 200, 200});
        QVERIFY2(contains(left, "ALPHA"), left.c_str());
        QVERIFY2(!contains(left, "BRAVO"), left.c_str());
        QVERIFY2(contains(right, "BRAVO"), right.c_str());
        QVERIFY2(!contains(right, "ALPHA"), right.c_str());

        domain::RectF media{};
        QVERIFY(test::pageops::readPageBox(result.bytes, 0, "MediaBox", media));
        QCOMPARE(media.width(), 200.0);
        QCOMPARE(media.height(), 200.0);

        assertQpdfClean(result.bytes, QStringLiteral("merge-2up"));
    }

    void fourUpFillsAllQuadrants() {
        const std::string source = test::pageops::makeFixturePdf(fourPages());

        MergePagesRequest request;
        request.pages = {0, 1, 2, 3};
        request.layout = domain::compose::MergeLayout::nUp(4);
        request.layout.pageSize = domain::SizeF{400.0, 800.0};  // 每格正好 200×400

        const MergePagesResult result = mergePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 1);

        // 第 0 格在左上，這是 N-up 最容易上下顛倒的地方。
        const struct {
            const char* text;
            domain::RectF area;
        } expected[] = {
            {"ALPHA", domain::RectF{0, 400, 200, 800}},
            {"BRAVO", domain::RectF{200, 400, 400, 800}},
            {"CHARLIE", domain::RectF{0, 0, 200, 400}},
            {"DELTA", domain::RectF{200, 0, 400, 400}},
        };
        for (const auto& item : expected) {
            const std::string found =
                test::pageops::textInArea(result.bytes, dir_->path(), 0, item.area);
            QVERIFY2(contains(found, item.text),
                     (std::string{"格內找不到 "} + item.text + "，實際是：" + found).c_str());
        }

        assertQpdfClean(result.bytes, QStringLiteral("merge-4up"));
    }

    void annotationsFollowTheContent() {
        std::vector<FixturePage> pages = {textPage("ALPHA"), textPage("BRAVO")};
        FixtureAnnotation highlight;
        highlight.rect = domain::RectF{20.0, 296.0, 120.0, 316.0};
        pages[0].annotations.push_back(highlight);
        // 第二頁也放一個，位置不同：只搬第一頁的實作會在這裡露餡。
        FixtureAnnotation second;
        second.rect = domain::RectF{40.0, 100.0, 140.0, 120.0};
        pages[1].annotations.push_back(second);

        const std::string source = test::pageops::makeFixturePdf(pages);

        MergePagesRequest request;
        request.pages = {0, 1};
        request.layout = domain::compose::MergeLayout::nUp(2);
        request.layout.pageSize = domain::SizeF{200.0, 200.0};  // 縮放 0.5

        const MergePagesResult result = mergePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.movedAnnotations, 2);

        const std::vector<test::pageops::AnnotationReadback> annotations =
            test::pageops::readAnnotations(result.bytes, 0);
        QCOMPARE(annotations.size(), std::size_t{2});

        // 第一頁的螢光筆：整體縮一半，落在左半格。
        QCOMPARE(annotations[0].rect.left, 10.0);
        QCOMPARE(annotations[0].rect.bottom, 148.0);
        QCOMPARE(annotations[0].rect.right, 60.0);
        QCOMPARE(annotations[0].rect.top, 158.0);

        // /QuadPoints 是螢光筆真正畫出來的形狀。只搬 /Rect 的實作在這裡就會被抓到。
        QCOMPARE(annotations[0].quadPoints.size(), std::size_t{8});
        QCOMPARE(annotations[0].quadPoints[0], 10.0);   // 左上 x
        QCOMPARE(annotations[0].quadPoints[1], 158.0);  // 左上 y
        QCOMPARE(annotations[0].quadPoints[6], 60.0);   // 右下 x
        QCOMPARE(annotations[0].quadPoints[7], 148.0);  // 右下 y

        // 第二頁的螢光筆：縮一半之後再右移一格（100 點）。
        QCOMPARE(annotations[1].rect.left, 120.0);
        QCOMPARE(annotations[1].rect.bottom, 50.0);
        QCOMPARE(annotations[1].rect.right, 170.0);
        QCOMPARE(annotations[1].rect.top, 60.0);

        assertQpdfClean(result.bytes, QStringLiteral("merge-annots"));
    }

    void sourcePagesDoNotPolluteEachOthersGraphicsState() {
        // 第一頁刻意留一個沒有 Q 的 q，並在裡面把填色設成紅色。
        // 這在真實檔案裡非常常見——單獨一頁時完全看不出差別。
        std::vector<FixturePage> pages = {textPage("ALPHA"), textPage("BRAVO")};
        pages[0].rawContentPrefix = "q\n1 0 0 rg\n10 10 40 40 re\nf\n";
        // 第二頁不設任何顏色，因此它的方塊必須是預設的黑色。
        pages[1].rawContentPrefix = "20 20 60 60 re\nf\n";

        const std::string source = test::pageops::makeFixturePdf(pages);

        MergePagesRequest request;
        request.pages = {0, 1};
        request.layout = domain::compose::MergeLayout::nUp(2);
        request.layout.pageSize = domain::SizeF{400.0, 400.0};  // 每格 200×400，縮放 1

        const MergePagesResult result = mergePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        const engine::PixelBufferPtr buffer = test::pageops::renderPage(result.bytes, dir_->path(), 0, 400);
        QVERIFY2(buffer != nullptr, "合併頁渲染失敗");

        const domain::SizeF pageSize{400.0, 400.0};
        // 前提檢查：第一頁的紅色方塊確實是紅的，否則下面那條斷言驗不到東西。
        const test::pageops::Pixel red = test::pageops::samplePage(*buffer, pageSize, domain::PointF{30.0, 30.0});
        QVERIFY2(red.r > 200 && red.g < 80 && red.b < 80,
                 QStringLiteral("來源頁 A 的方塊不是紅色：%1,%2,%3")
                     .arg(red.r).arg(red.g).arg(red.b).toUtf8().constData());

        // 第二頁的方塊在合併頁的 (220,20)–(280,80)。它必須是黑的：
        // 如果 A 少掉的那個 Q 外溢過來，這裡會變成紅色。
        const test::pageops::Pixel isolated = test::pageops::samplePage(*buffer, pageSize, domain::PointF{250.0, 50.0});
        QVERIFY2(isolated.r < 80 && isolated.g < 80 && isolated.b < 80,
                 QStringLiteral("來源頁 A 的圖形狀態污染了 B：%1,%2,%3")
                     .arg(isolated.r).arg(isolated.g).arg(isolated.b).toUtf8().constData());

        // 結構面的佐證：來源頁的運算子不該被串接進合併頁自己的內容串流，
        // 它們應該在各自的 Form XObject 裡。
        const std::string content = test::pageops::readPageContent(result.bytes, 0);
        QVERIFY2(!contains(content, "1 0 0 rg"), content.c_str());
        QVERIFY2(contains(content, " Do"), content.c_str());
    }

    void rotatedSourcePageIsUpright() {
        std::vector<FixturePage> pages = {textPage("ALPHA"), textPage("BRAVO")};
        pages[1].rotate = 90;  // 顯示時順時針轉 90 度，內容串流本身沒有轉

        const std::string source = test::pageops::makeFixturePdf(pages);

        MergePagesRequest request;
        request.pages = {0, 1};
        request.layout = domain::compose::MergeLayout::nUp(2);
        request.layout.pageSize = domain::SizeF{400.0, 400.0};

        const MergePagesResult result = mergePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        // 轉過的頁面是 400×200，等比塞進 200×400 的格子後只有 200×100，
        // 會垂直置中。重點不是精確位置，而是它落在右半格而不是溢出去。
        const std::string right = test::pageops::textInArea(result.bytes, dir_->path(), 0,
                                                            domain::RectF{200, 0, 400, 400});
        QVERIFY2(contains(right, "BRAVO"), right.c_str());

        assertQpdfClean(result.bytes, QStringLiteral("merge-rotate"));
    }

    void invalidRequestsAreRejectedNotGuessed() {
        const std::string source = test::pageops::makeFixturePdf(fourPages());

        MergePagesRequest empty;
        empty.layout = domain::compose::MergeLayout::nUp(2);
        QCOMPARE(mergePages(source, empty).status, PageOpsStatus::InvalidRequest);

        MergePagesRequest duplicated;
        duplicated.pages = {0, 0};
        duplicated.layout = domain::compose::MergeLayout::nUp(2);
        QCOMPARE(mergePages(source, duplicated).status, PageOpsStatus::InvalidRequest);

        MergePagesRequest outOfRange;
        outOfRange.pages = {0, 9};
        outOfRange.layout = domain::compose::MergeLayout::nUp(2);
        QCOMPARE(mergePages(source, outOfRange).status, PageOpsStatus::PageOutOfRange);

        MergePagesRequest overfull;
        overfull.pages = {0, 1, 2};
        overfull.layout = domain::compose::MergeLayout::nUp(2);
        const MergePagesResult result = mergePages(source, overfull);
        QCOMPARE(result.status, PageOpsStatus::LayoutRejected);
        QCOMPARE(result.compose, domain::compose::ComposeStatus::TooManySources);
    }

    void remainingPagesSurviveTheMerge() {
        const std::string source = test::pageops::makeFixturePdf(fourPages());

        MergePagesRequest request;
        request.pages = {1, 2};
        request.layout = domain::compose::MergeLayout::nUp(2);
        request.layout.pageSize = domain::SizeF{400.0, 400.0};

        const MergePagesResult result = mergePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 3);          // 4 頁併掉 2 頁換成 1 頁
        QCOMPARE(result.mergedPageIndex, 1);    // 插在第一個來源頁原本的位置

        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 0), "ALPHA"));
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 2), "DELTA"));
    }

private:
    void assertQpdfClean(const std::string& bytes, const QString& name) {
        const QString path = dir_->path() + QStringLiteral("/%1.pdf").arg(name);
        QVERIFY(test::pageops::writeBytes(path, bytes));
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        if (check.status == test::QpdfStatus::NotAvailable) {
            QSKIP("找不到 qpdf，結構檢查略過");
        }
        QVERIFY2(check.clean(), check.output.toUtf8().constData());
    }

    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestPageMerge)
#include "test_page_merge.moc"
