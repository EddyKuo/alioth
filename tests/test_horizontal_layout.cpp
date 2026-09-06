// Ribbon Layout：頁面左右排列（PRD-VIEW-011）。
//
// 這個模式的用途是比對相鄰兩頁的內容——垂直連續模式下，相鄰兩頁在畫面上
// 永遠隔著一整頁的高度，根本比不了。因此本測試的重點是「相鄰頁真的相鄰」
// 以及座標往返在水平排列下仍然自洽。

#include <QtTest>

#include "domain/page_layout.h"

using namespace alioth::domain;

namespace {

PageLayout makeLayout(const std::vector<SizeF>& sizes, const LayoutOptions& options,
                      double scale = 1.0, std::int32_t viewportWidth = 800) {
    PageLayout layout;
    layout.setPageSizes(sizes);
    layout.update(scale, Rotation::None, options, viewportWidth);
    return layout;
}

LayoutOptions horizontal() {
    LayoutOptions options;
    options.mode = LayoutMode::Horizontal;
    options.pageGapPx = 10.0;
    return options;
}

}  // namespace

class TestHorizontalLayout : public QObject {
    Q_OBJECT

private slots:
    void allPagesShareOneRow() {
        const PageLayout layout =
            makeLayout({{200, 300}, {200, 300}, {200, 300}, {200, 300}}, horizontal());
        QCOMPARE(layout.placements().size(), std::size_t{4});

        const auto& places = layout.placements();
        for (std::size_t i = 1; i < places.size(); ++i) {
            // 同一列：垂直位置相同（等高頁面），水平位置遞增。
            QCOMPARE(places[i].rect.y, places[0].rect.y);
            QVERIFY(places[i].rect.x > places[i - 1].rect.x);
        }
    }

    void adjacentPagesAreSeparatedOnlyByTheGap() {
        const PageLayout layout = makeLayout({{200, 300}, {200, 300}}, horizontal());
        const auto& places = layout.placements();
        const int gapBetween = places[1].rect.x - (places[0].rect.x + places[0].rect.width);
        QCOMPARE(gapBetween, 10);
    }

    void pagesOfDifferentHeightsAreVerticallyCentred() {
        // 靠上對齊會讓一列頁面看起來像參差的鋸齒，而這個模式的重點正是並排比對。
        const PageLayout layout = makeLayout({{200, 400}, {200, 200}}, horizontal());
        const auto& places = layout.placements();
        const int tallCentre = places[0].rect.y + places[0].rect.height / 2;
        const int shortCentre = places[1].rect.y + places[1].rect.height / 2;
        QVERIFY2(std::abs(tallCentre - shortCentre) <= 1,
                 qPrintable(QStringLiteral("中線差 %1").arg(std::abs(tallCentre - shortCentre))));
    }

    void contentIsWiderThanTallForManyPages() {
        const PageLayout layout =
            makeLayout({{200, 300}, {200, 300}, {200, 300}, {200, 300}, {200, 300}}, horizontal());
        QVERIFY(layout.contentSize().width > layout.contentSize().height);
    }

    void pageRectLookupMatchesPlacement() {
        const PageLayout layout = makeLayout({{200, 300}, {150, 250}, {300, 400}}, horizontal());
        for (const PagePlacement& placement : layout.placements()) {
            const RectI byIndex = layout.pageRect(placement.pageIndex);
            QCOMPARE(byIndex.x, placement.rect.x);
            QCOMPARE(byIndex.y, placement.rect.y);
            QCOMPARE(byIndex.width, placement.rect.width);
        }
    }

    void coordinateRoundTripHoldsUnderHorizontalLayout() {
        // 座標往返自洽是所有版面模式的共同判準：頁內座標 → 文件座標 → 頁內座標
        // 不得漂移，否則點擊位置與畫面上看到的位置會對不上。
        const PageLayout layout = makeLayout({{200, 300}, {200, 300}, {200, 300}}, horizontal(),
                                             1.5);
        const RectI rect = layout.pageRect(2);
        const PointF documentPoint{static_cast<double>(rect.x) + 37.0,
                                   static_cast<double>(rect.y) + 51.0};
        const PointF local = layout.toPageLocal(2, documentPoint);
        QVERIFY(std::abs(local.x - 37.0) < 0.001);
        QVERIFY(std::abs(local.y - 51.0) < 0.001);
    }

    void visiblePagesReturnsOnlyWhatIntersectsTheViewport() {
        const PageLayout layout = makeLayout(
            {{200, 300}, {200, 300}, {200, 300}, {200, 300}, {200, 300}}, horizontal());
        // 只看得到最前面那段。
        const std::vector<PagePlacement> visible =
            layout.visiblePages(RectI{0, 0, 260, 400});
        QVERIFY(!visible.empty());
        QVERIFY(visible.size() < layout.placements().size());
        QCOMPARE(visible.front().pageIndex, 0);
    }

    void rightToLeftReversesTheVisualOrder() {
        LayoutOptions options = horizontal();
        options.rightToLeft = true;
        const PageLayout layout = makeLayout({{200, 300}, {200, 300}, {200, 300}}, options);

        // 頁碼順序不變，但第 0 頁應該在最右邊（PRD-VIEW-012 的規則同樣適用）。
        QCOMPARE(layout.pageRect(0).x > layout.pageRect(2).x, true);
    }

    void emptyDocumentProducesNoPlacements() {
        const PageLayout layout = makeLayout({}, horizontal());
        QVERIFY(layout.placements().empty());
        QCOMPARE(layout.pageCount(), 0);
    }

    void singlePageBehavesLikeAnyOtherMode() {
        const PageLayout layout = makeLayout({{200, 300}}, horizontal());
        QCOMPARE(layout.placements().size(), std::size_t{1});
        QCOMPARE(layout.placements()[0].pageIndex, 0);
    }
};

QTEST_MAIN(TestHorizontalLayout)
#include "test_horizontal_layout.moc"
