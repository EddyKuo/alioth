// 版面計算測試。
//
// 連續 / 雙頁 / 封面獨立 / 右至左四個維度交叉起來有十幾種組合，
// 而版面錯了的症狀是「頁面疊在一起」或「捲不到最後一頁」——都不會崩潰，
// 只會被當成偶發的顯示問題。所以這裡逐組合驗。

#include <QtTest>

#include "domain/page_layout.h"

using namespace alioth::domain;

namespace {

std::vector<SizeF> a4Pages(int count) {
    return std::vector<SizeF>(static_cast<std::size_t>(count), SizeF{595.0, 842.0});
}

}  // namespace

class TestPageLayout : public QObject {
    Q_OBJECT

private slots:
    void continuousStacksPagesVertically() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(3));
        LayoutOptions options;
        options.mode = LayoutMode::Continuous;
        options.pageGapPx = 10.0;
        layout.update(1.0, Rotation::None, options, 800);

        QCOMPARE(layout.pageCount(), 3);
        const RectI first = layout.pageRect(0);
        const RectI second = layout.pageRect(1);
        QCOMPARE(first.width, 595);
        QCOMPARE(first.height, 842);
        // 第二頁必須在第一頁下方，且間隔正好是設定的間隙。
        QCOMPARE(second.y, first.bottom() + 10);
        QCOMPARE(second.x, first.x);
        // 內容高度要蓋住最後一頁，否則捲不到底。
        QVERIFY(layout.contentSize().height >= layout.pageRect(2).bottom());
    }

    void singlePageModeOnlyLaysOutCurrentPage() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(10));
        LayoutOptions options;
        options.mode = LayoutMode::SinglePage;
        layout.setCurrentPage(4);
        layout.update(1.0, Rotation::None, options, 800);

        QCOMPARE(layout.pageCount(), 1);
        QCOMPARE(layout.placements().front().pageIndex, 4);
        // 捲軸範圍應該只有一頁高，而不是十頁。
        QVERIFY(layout.contentSize().height < 900.0 + 100.0);
    }

    void twoPageContinuousPairsPagesWithSeparateCover() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(5));
        LayoutOptions options;
        options.mode = LayoutMode::TwoPageContinuous;
        options.coverPageSeparate = true;
        layout.update(1.0, Rotation::None, options, 1400);

        // 封面獨立：第 0 頁自己一列，之後 (1,2)、(3,4) 成對。
        QCOMPARE(layout.pageRect(1).y, layout.pageRect(2).y);
        QCOMPARE(layout.pageRect(3).y, layout.pageRect(4).y);
        QVERIFY(layout.pageRect(0).y < layout.pageRect(1).y);
        QVERIFY(layout.pageRect(1).x < layout.pageRect(2).x);
    }

    void twoPageContinuousWithoutSeparateCoverPairsFromFirstPage() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(4));
        LayoutOptions options;
        options.mode = LayoutMode::TwoPageContinuous;
        options.coverPageSeparate = false;
        layout.update(1.0, Rotation::None, options, 1400);

        QCOMPARE(layout.pageRect(0).y, layout.pageRect(1).y);
        QCOMPARE(layout.pageRect(2).y, layout.pageRect(3).y);
    }

    void rightToLeftSwapsVisualOrderNotPageOrder() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(3));
        LayoutOptions options;
        options.mode = LayoutMode::TwoPageContinuous;
        options.coverPageSeparate = false;
        options.rightToLeft = true;
        layout.update(1.0, Rotation::None, options, 1400);

        // 右至左：第 0 頁在右邊，但 pageRect(0) 仍然指向第 0 頁。
        QVERIFY(layout.pageRect(0).x > layout.pageRect(1).x);
        QCOMPARE(layout.pageRect(0).y, layout.pageRect(1).y);
    }

    void rotationSwapsPageExtent() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(1));
        LayoutOptions options;
        options.mode = LayoutMode::Continuous;
        layout.update(1.0, Rotation::Cw90, options, 1000);

        const RectI rect = layout.pageRect(0);
        QCOMPARE(rect.width, 842);
        QCOMPARE(rect.height, 595);
    }

    void visiblePagesOnlyReturnsIntersectingPages() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(20));
        LayoutOptions options;
        options.mode = LayoutMode::Continuous;
        layout.update(1.0, Rotation::None, options, 800);

        // 只看一個 A4 高度的視窗，不該回報二十頁——否則排程會為看不到的頁面算圖磚。
        const auto visible = layout.visiblePages(RectI{0, 0, 800, 600});
        QVERIFY(!visible.empty());
        QVERIFY(visible.size() <= 2);
        QCOMPARE(visible.front().pageIndex, 0);
    }

    void pageAtViewportCentreTracksScrolling() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(5));
        LayoutOptions options;
        options.mode = LayoutMode::Continuous;
        layout.update(1.0, Rotation::None, options, 800);

        QCOMPARE(layout.pageAtViewportCenter(RectI{0, 0, 800, 600}), 0);

        const RectI third = layout.pageRect(2);
        const RectI viewport{0, third.y + third.height / 2 - 300, 800, 600};
        QCOMPARE(layout.pageAtViewportCenter(viewport), 2);
    }

    void scalingScalesEveryPage() {
        PageLayout layout;
        layout.setPageSizes(a4Pages(2));
        LayoutOptions options;
        options.mode = LayoutMode::Continuous;
        layout.update(2.0, Rotation::None, options, 2000);

        QCOMPARE(layout.pageRect(0).width, 1190);
        QCOMPARE(layout.pageRect(0).height, 1684);
    }

    void emptyDocumentProducesEmptyLayout() {
        PageLayout layout;
        layout.setPageSizes({});
        layout.update(1.0, Rotation::None, LayoutOptions{}, 800);
        QCOMPARE(layout.pageCount(), 0);
        QCOMPARE(layout.contentSize().height, 0.0);
        QVERIFY(layout.pageRect(0).isEmpty());
        QVERIFY(layout.visiblePages(RectI{0, 0, 800, 600}).empty());
    }
};

QTEST_APPLESS_MAIN(TestPageLayout)
#include "test_page_layout.moc"
