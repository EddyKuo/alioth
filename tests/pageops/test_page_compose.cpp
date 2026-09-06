// 頁面合成的版面數學（WBS 12，PRD-PAGE-007 / 008 / 010 / 011）。
//
// 這一支刻意不碰 PDF：版面算錯與「寫進檔案時寫錯」是兩種不同的缺陷，
// 混在同一支測試裡的話，任何一個失敗都要先花時間排除另一個的嫌疑。

#include <QtTest>

#include "domain/page_compose.h"

using namespace alioth::domain;
using namespace alioth::domain::compose;

namespace {

bool nearly(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

bool nearlyRect(const RectF& a, const RectF& b, double tolerance = 1e-9) {
    return nearly(a.left, b.left, tolerance) && nearly(a.bottom, b.bottom, tolerance) &&
           nearly(a.right, b.right, tolerance) && nearly(a.top, b.top, tolerance);
}

}  // namespace

class TestPageCompose : public QObject {
    Q_OBJECT

private slots:
    void matrixOrderMatchesPdfConvention() {
        // 先平移再縮放，與先縮放再平移是不同的結果。順序寫反不會崩潰，
        // 只會讓整頁偏掉，所以這裡把兩種順序都釘住。
        const Matrix a = concat(translation(10.0, 20.0), scaling(2.0, 3.0));
        const PointF p = apply(a, PointF{1.0, 1.0});
        QVERIFY(nearly(p.x, 22.0));
        QVERIFY(nearly(p.y, 63.0));

        const Matrix b = concat(scaling(2.0, 3.0), translation(10.0, 20.0));
        const PointF q = apply(b, PointF{1.0, 1.0});
        QVERIFY(nearly(q.x, 12.0));
        QVERIFY(nearly(q.y, 23.0));
    }

    void twoUpPlacesPagesLeftAndRight() {
        MergeLayout layout = MergeLayout::nUp(2);
        layout.pageSize = SizeF{400.0, 400.0};
        const std::vector<RectF> sources(2, RectF{0.0, 0.0, 200.0, 400.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY2(plan.ok(), describe(plan.status));
        QCOMPARE(plan.placements.size(), std::size_t{2});

        // 兩格各 200×400，來源正好 200×400，因此不需要縮放。
        QVERIFY(nearly(plan.placements[0].scaleX, 1.0));
        QVERIFY(nearlyRect(plan.placements[0].placedBounds, RectF{0.0, 0.0, 200.0, 400.0}));
        QVERIFY(nearlyRect(plan.placements[1].placedBounds, RectF{200.0, 0.0, 400.0, 400.0}));
    }

    void fourUpFillsQuadrantsInReadingOrder() {
        MergeLayout layout = MergeLayout::nUp(4);
        layout.pageSize = SizeF{400.0, 400.0};
        const std::vector<RectF> sources(4, RectF{0.0, 0.0, 200.0, 200.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY2(plan.ok(), describe(plan.status));

        // 第 0 格必須在**左上**：格子編號是閱讀順序，PDF 的 Y 軸卻向上，
        // 這是 N-up 版面最容易上下顛倒的地方。
        QVERIFY(nearlyRect(plan.placements[0].placedBounds, RectF{0.0, 200.0, 200.0, 400.0}));
        QVERIFY(nearlyRect(plan.placements[1].placedBounds, RectF{200.0, 200.0, 400.0, 400.0}));
        QVERIFY(nearlyRect(plan.placements[2].placedBounds, RectF{0.0, 0.0, 200.0, 200.0}));
        QVERIFY(nearlyRect(plan.placements[3].placedBounds, RectF{200.0, 0.0, 400.0, 200.0}));
    }

    void columnMajorFillsDownFirst() {
        MergeLayout layout = MergeLayout::nUp(4);
        layout.order = CellOrder::ColumnMajor;
        layout.pageSize = SizeF{400.0, 400.0};
        const std::vector<RectF> sources(4, RectF{0.0, 0.0, 200.0, 200.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY(plan.ok());
        QCOMPARE(plan.placements[1].row, 1);
        QCOMPARE(plan.placements[1].column, 0);
    }

    void containKeepsAspectAndCenters() {
        MergeLayout layout = MergeLayout::nUp(2);
        layout.pageSize = SizeF{400.0, 400.0};
        // 來源是 100×400 的細長頁，格子是 200×400：等比縮放後寬度只有 100，
        // 必須水平置中而不是靠左。
        const std::vector<RectF> sources(2, RectF{0.0, 0.0, 100.0, 400.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY(plan.ok());
        QVERIFY(nearly(plan.placements[0].scaleX, 1.0));
        QVERIFY(nearlyRect(plan.placements[0].placedBounds, RectF{50.0, 0.0, 150.0, 400.0}));
    }

    void stretchFillsCellAndDistorts() {
        MergeLayout layout = MergeLayout::nUp(2);
        layout.fit = CellFit::Stretch;
        layout.pageSize = SizeF{400.0, 400.0};
        const std::vector<RectF> sources(2, RectF{0.0, 0.0, 100.0, 400.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY(plan.ok());
        QVERIFY(nearly(plan.placements[0].scaleX, 2.0));
        QVERIFY(nearly(plan.placements[0].scaleY, 1.0));
    }

    void marginAndGutterShrinkCells() {
        MergeLayout layout = MergeLayout::nUp(2);
        layout.pageSize = SizeF{440.0, 420.0};
        layout.marginPt = 10.0;
        layout.gutterPt = 20.0;
        const std::vector<RectF> sources(2, RectF{0.0, 0.0, 200.0, 400.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY(plan.ok());
        QVERIFY(nearlyRect(plan.placements[0].cell, RectF{10.0, 10.0, 210.0, 410.0}));
        QVERIFY(nearlyRect(plan.placements[1].cell, RectF{230.0, 10.0, 430.0, 410.0}));
    }

    void nonZeroOriginSourceIsFoldedIntoMatrix() {
        // 這是 PRD-PAGE-011 與 007 交界的坑：來源頁的 MediaBox 原點不是 (0,0) 時，
        // 版面必須以「可見範圍」對齊，而不是把絕對座標直接縮放。
        MergeLayout layout = MergeLayout::nUp(2);
        layout.pageSize = SizeF{400.0, 400.0};
        const std::vector<RectF> sources(2, RectF{50.0, 100.0, 250.0, 500.0});

        const MergePlan plan = planMerge(layout, sources);
        QVERIFY(plan.ok());
        // 來源的左下角 (50,100) 必須落在格子的左下角 (0,0)。
        const PointF corner = apply(plan.placements[0].matrix, PointF{50.0, 100.0});
        QVERIFY(nearly(corner.x, 0.0));
        QVERIFY(nearly(corner.y, 0.0));
        QVERIFY(nearlyRect(plan.placements[1].placedBounds, RectF{200.0, 0.0, 400.0, 400.0}));
    }

    void mergeRejectsOverfilledLayout() {
        MergeLayout layout = MergeLayout::nUp(2);
        const std::vector<RectF> sources(3, RectF{0.0, 0.0, 200.0, 400.0});
        QCOMPARE(planMerge(layout, sources).status, ComposeStatus::TooManySources);

        QCOMPARE(planMerge(layout, {}).status, ComposeStatus::EmptySource);

        MergeLayout tight = MergeLayout::nUp(2);
        tight.pageSize = SizeF{40.0, 40.0};
        tight.marginPt = 30.0;
        QCOMPARE(planMerge(tight, std::vector<RectF>(1, RectF{0, 0, 10, 10})).status,
                 ComposeStatus::InvalidPageSize);
    }

    void overlayAnchorsProduceExpectedOrigins() {
        const RectF base{0.0, 0.0, 400.0, 400.0};
        const RectF overlay{0.0, 0.0, 100.0, 50.0};

        OverlayOptions options;
        options.anchor = OverlayAnchor::TopLeft;
        QVERIFY(nearlyRect(planOverlay(options, base, overlay).bounds,
                           RectF{0.0, 350.0, 100.0, 400.0}));

        options.anchor = OverlayAnchor::Center;
        QVERIFY(nearlyRect(planOverlay(options, base, overlay).bounds,
                           RectF{150.0, 175.0, 250.0, 225.0}));

        options.anchor = OverlayAnchor::BottomRight;
        QVERIFY(nearlyRect(planOverlay(options, base, overlay).bounds,
                           RectF{300.0, 0.0, 400.0, 50.0}));
    }

    void overlayStretchFillsBaseBox() {
        const RectF base{0.0, 0.0, 400.0, 400.0};
        const RectF overlay{0.0, 0.0, 100.0, 50.0};

        OverlayOptions options;
        options.anchor = OverlayAnchor::Stretch;
        const OverlayPlacement placement = planOverlay(options, base, overlay);
        QVERIFY(placement.ok());
        QVERIFY(nearly(placement.matrix.a, 4.0));
        QVERIFY(nearly(placement.matrix.d, 8.0));
        QVERIFY(nearlyRect(placement.bounds, base));
    }

    void overlayAlignsAgainstShiftedBasePage() {
        // 底頁的 MediaBox 原點不是 (0,0)：置中必須以可見範圍為準。
        const RectF base{100.0, 200.0, 500.0, 600.0};
        const RectF overlay{0.0, 0.0, 100.0, 50.0};

        OverlayOptions options;
        options.anchor = OverlayAnchor::Center;
        QVERIFY(nearlyRect(planOverlay(options, base, overlay).bounds,
                           RectF{250.0, 375.0, 350.0, 425.0}));
    }

    void boxSettingsClampChildrenToMedia() {
        PageBoxSettings settings;
        settings.media = RectF{0.0, 0.0, 400.0, 400.0};
        settings.crop = RectF{-10.0, -10.0, 300.0, 300.0};

        const ResolvedPageBoxes resolved = resolvePageBoxes(settings, RectF{0, 0, 100, 100});
        QVERIFY(resolved.ok());
        QVERIFY(resolved.clamped);
        QVERIFY(nearlyRect(*resolved.crop, RectF{0.0, 0.0, 300.0, 300.0}));

        settings.clampToMedia = false;
        QCOMPARE(resolvePageBoxes(settings, RectF{0, 0, 100, 100}).status,
                 ComposeStatus::BoxOutsideMedia);
    }

    void boxSettingsLeaveUnsetBoxesAlone() {
        PageBoxSettings settings;
        settings.trim = RectF{10.0, 10.0, 90.0, 90.0};

        const ResolvedPageBoxes resolved = resolvePageBoxes(settings, RectF{0, 0, 100, 100});
        QVERIFY(resolved.ok());
        QVERIFY(!resolved.crop.has_value());
        QVERIFY(!resolved.art.has_value());
        QVERIFY(resolved.trim.has_value());
        QVERIFY(nearlyRect(resolved.media, RectF{0, 0, 100, 100}));

        QCOMPARE(resolvePageBoxes(PageBoxSettings{}, RectF{0, 0, 100, 100}).status,
                 ComposeStatus::NothingToDo);
    }

    void normalizationComputesShiftAndNewMedia() {
        const NormalizationPlan plan = planNormalization(RectF{20.0, -30.0, 620.0, 812.0});
        QVERIFY(plan.ok());
        QVERIFY(plan.needed);
        QVERIFY(nearly(plan.dx, -20.0));
        QVERIFY(nearly(plan.dy, 30.0));
        QVERIFY(nearlyRect(plan.media, RectF{0.0, 0.0, 600.0, 842.0}));

        // 已經在原點的頁面不該被重寫：那會讓每次開檔存檔都長大一段。
        const NormalizationPlan noop = planNormalization(RectF{0.0, 0.0, 600.0, 842.0});
        QVERIFY(noop.ok());
        QVERIFY(!noop.needed);
    }

    void rotationMatrixMapsCornersConsistently() {
        const RectF box{10.0, 20.0, 210.0, 420.0};  // 200×400

        const SizeF ninety = rotatedSize(box, 90);
        QVERIFY(nearly(ninety.width, 400.0));
        QVERIFY(nearly(ninety.height, 200.0));

        const Matrix m = rotationMatrix(box, 90);
        const RectF mapped = apply(m, box);
        QVERIFY(nearlyRect(mapped, RectF{0.0, 0.0, 400.0, 200.0}, 1e-9));

        // 順時針 90 度後，原本的左下角應該在左上角。
        const PointF bottomLeft = apply(m, PointF{box.left, box.bottom});
        QVERIFY(nearly(bottomLeft.x, 0.0));
        QVERIFY(nearly(bottomLeft.y, 200.0));

        QVERIFY(nearlyRect(apply(rotationMatrix(box, 180), box), RectF{0, 0, 200, 400}, 1e-9));
        QVERIFY(nearlyRect(apply(rotationMatrix(box, 270), box), RectF{0, 0, 400, 200}, 1e-9));
        QVERIFY(nearlyRect(apply(rotationMatrix(box, 0), box), RectF{0, 0, 200, 400}, 1e-9));
    }
};

QTEST_APPLESS_MAIN(TestPageCompose)
#include "test_page_compose.moc"
