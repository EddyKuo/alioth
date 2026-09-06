// 列印計畫：縮放模式、海報分割、張數（PRD-IO-008）。
//
// 印表機在 CI 上不存在，但列印最貴的錯誤全部發生在計畫階段：少印一頁、
// 頁面被壓扁、海報接縫錯位。這些都是純資料，可以逐項斷言。

#include <QtTest>

#include "app/print/print_plan.h"

using namespace alioth::app::print;

namespace {

constexpr double kA4Width = 595.0;
constexpr double kA4Height = 842.0;

QRectF a4Printable() { return QRectF{0.0, 0.0, kA4Width, kA4Height}; }

std::vector<QSizeF> uniformPages(int count, double w = 200.0, double h = 400.0) {
    return std::vector<QSizeF>(static_cast<std::size_t>(count), QSizeF{w, h});
}

}  // namespace

class TestPrintPlan : public QObject {
    Q_OBJECT

private slots:
    void fitToPaperPreservesAspectRatio() {
        PrintOptions options;
        options.scaleMode = ScaleMode::FitToPaper;

        const QRectF target = computePageTargetRect(QSizeF{200.0, 400.0}, a4Printable(), options);
        // 兩軸取同一倍率：長寬比必須與原頁面一致，否則就是變形。
        QVERIFY(qFuzzyCompare(target.width() / target.height(), 200.0 / 400.0));
        QVERIFY(qFuzzyCompare(target.height(), kA4Height));
        QVERIFY(target.width() <= kA4Width + 1e-9);
    }

    void fitToPaperCentresThePage() {
        PrintOptions options;
        options.scaleMode = ScaleMode::FitToPaper;
        const QRectF target = computePageTargetRect(QSizeF{200.0, 400.0}, a4Printable(), options);
        QVERIFY(qFuzzyCompare(target.left(), (kA4Width - target.width()) / 2.0));
        QVERIFY(qFuzzyCompare(kA4Width - target.right(), target.left()));
    }

    void actualSizeIgnoresPaper() {
        PrintOptions options;
        options.scaleMode = ScaleMode::ActualSize;
        options.autoRotate = false;
        const QRectF target = computePageTargetRect(QSizeF{200.0, 400.0}, a4Printable(), options);
        QCOMPARE(target.size(), QSizeF(200.0, 400.0));
    }

    void actualSizeOnOversizedPageOverflowsAndGetsClipped() {
        PrintOptions options;
        options.scaleMode = ScaleMode::ActualSize;
        options.autoRotate = false;

        const PrintPlan plan = buildPrintPlan(uniformPages(1, 1200.0, 1600.0), a4Printable(), options);
        QVERIFY(plan.valid);
        QCOMPARE(plan.sheetCount(), 1);
        // 頁面比紙大時原點為負，裁切框退回可列印區——內容被裁掉而不是被縮小，
        // 這正是「實際大小」的語意。
        QVERIFY(plan.sheets[0].pageRectPt.left() < 0.0);
        QCOMPARE(plan.sheets[0].clipRectPt, a4Printable());
    }

    void customPercentScalesUniformly() {
        PrintOptions options;
        options.scaleMode = ScaleMode::Custom;
        options.customScalePercent = 50.0;
        options.autoRotate = false;
        const QRectF target = computePageTargetRect(QSizeF{200.0, 400.0}, a4Printable(), options);
        QCOMPARE(target.size(), QSizeF(100.0, 200.0));
    }

    void enlargeSmallPagesCanBeDisabled() {
        PrintOptions options;
        options.scaleMode = ScaleMode::FitToPaper;
        options.enlargeSmallPages = false;
        options.autoRotate = false;
        const QRectF target = computePageTargetRect(QSizeF{200.0, 400.0}, a4Printable(), options);
        QCOMPARE(target.size(), QSizeF(200.0, 400.0));
    }

    void autoRotateSwapsAxesForLandscapePages() {
        PrintOptions options;
        options.scaleMode = ScaleMode::FitToPaper;
        options.autoRotate = true;

        bool rotated = false;
        const QRectF target =
            computePageTargetRect(QSizeF{400.0, 200.0}, a4Printable(), options, &rotated);
        QVERIFY(rotated);
        // 轉正之後長寬比是 200:400，與直式紙張相符，才吃得滿整張紙的高度。
        QVERIFY(qFuzzyCompare(target.height(), kA4Height));
        QVERIFY(qFuzzyCompare(target.width() / target.height(), 200.0 / 400.0));
    }

    void autoRotateCanBeDisabled() {
        PrintOptions options;
        options.autoRotate = false;
        bool rotated = true;
        const QRectF ignored =
            computePageTargetRect(QSizeF{400.0, 200.0}, a4Printable(), options, &rotated);
        QVERIFY(!ignored.isEmpty());
        QVERIFY(!rotated);
    }

    void degeneratePageSizeYieldsEmptyRect() {
        PrintOptions options;
        QVERIFY(computePageTargetRect(QSizeF{0.0, 400.0}, a4Printable(), options).isEmpty());
        QVERIFY(computePageTargetRect(QSizeF{200.0, 400.0}, QRectF{}, options).isEmpty());
    }

    // ---- 海報分割 ----

    void posterSlicesCoverThePageWithoutOverlap() {
        int columns = 0;
        int rows = 0;
        const std::vector<QRectF> slices =
            computePosterSlices(QSizeF{1000.0, 1000.0}, QSizeF{400.0, 400.0}, 0.0, &columns, &rows);
        QCOMPARE(columns, 3);
        QCOMPARE(rows, 3);
        QCOMPARE(slices.size(), std::size_t{9});

        QCOMPARE(slices[0], QRectF(0.0, 0.0, 400.0, 400.0));
        QCOMPARE(slices[1], QRectF(400.0, 0.0, 400.0, 400.0));
        // 最後一欄只剩 200 點寬，切片必須跟著縮短，否則會多出一段空白且接縫錯位。
        QCOMPARE(slices[2], QRectF(800.0, 0.0, 200.0, 400.0));
        QCOMPARE(slices[8], QRectF(800.0, 800.0, 200.0, 200.0));
    }

    void posterOverlapReducesStepAndStillCoversThePage() {
        int columns = 0;
        int rows = 0;
        const std::vector<QRectF> slices = computePosterSlices(
            QSizeF{1000.0, 1000.0}, QSizeF{400.0, 400.0}, 100.0, &columns, &rows);
        QCOMPARE(columns, 3);
        QCOMPARE(rows, 3);
        QCOMPARE(slices[0].left(), 0.0);
        QCOMPARE(slices[1].left(), 300.0);
        QCOMPARE(slices[2].left(), 600.0);
        // 最右一欄的右緣必須抵達頁面邊界，不能留下沒印到的長條。
        QCOMPARE(slices[2].right(), 1000.0);
    }

    void pageSmallerThanSheetProducesSingleSlice() {
        int columns = 0;
        int rows = 0;
        const std::vector<QRectF> slices =
            computePosterSlices(QSizeF{300.0, 300.0}, QSizeF{400.0, 400.0}, 20.0, &columns, &rows);
        QCOMPARE(columns, 1);
        QCOMPARE(rows, 1);
        QCOMPARE(slices.size(), std::size_t{1});
        QCOMPARE(slices[0], QRectF(0.0, 0.0, 300.0, 300.0));
    }

    void excessiveOverlapIsClampedInsteadOfLoopingForever() {
        int columns = 0;
        int rows = 0;
        const std::vector<QRectF> slices = computePosterSlices(
            QSizeF{1000.0, 1000.0}, QSizeF{400.0, 400.0}, 10000.0, &columns, &rows);
        QVERIFY(columns >= 1);
        QVERIFY(!slices.empty());
        QVERIFY(slices.size() < 1000);
    }

    // ---- 完整計畫 ----

    void sheetCountFollowsPageRange() {
        PrintOptions options;
        options.pageRangeSpec = QStringLiteral("2-4");
        const PrintPlan plan = buildPrintPlan(uniformPages(10), a4Printable(), options);
        QVERIFY(plan.valid);
        QCOMPARE(plan.sheetCount(), 3);
        QCOMPARE(plan.pages, std::vector<int>({1, 2, 3}));
    }

    void reverseOrderFlipsSheetOrderNotPageIdentity() {
        PrintOptions options;
        options.pageRangeSpec = QStringLiteral("1-3");
        options.reverseOrder = true;
        const PrintPlan plan = buildPrintPlan(uniformPages(5), a4Printable(), options);
        QCOMPARE(plan.pages, std::vector<int>({2, 1, 0}));
        QCOMPARE(plan.sheets[0].pageIndex, 2);
        QCOMPARE(plan.sheets[0].printSequence, 0);
    }

    void posterMultipliesSheetsPerPage() {
        PrintOptions options;
        options.scaleMode = ScaleMode::FitToPaper;
        options.poster.enabled = true;
        options.poster.zoomPercent = 200.0;
        options.poster.overlapPt = 0.0;

        const PrintPlan plan = buildPrintPlan(uniformPages(3), a4Printable(), options);
        QVERIFY(plan.valid);
        // 放大兩倍後頁面是 2 欄 × 2 列，三頁共 12 張。
        QCOMPARE(plan.sheets[0].posterColumns, 2);
        QCOMPARE(plan.sheets[0].posterRows, 2);
        QCOMPARE(plan.sheetCount(), 12);
    }

    void posterSheetOriginShiftsSoTheSameDrawPathWorks() {
        PrintOptions options;
        options.scaleMode = ScaleMode::FitToPaper;
        options.poster.enabled = true;
        options.poster.zoomPercent = 200.0;
        options.poster.overlapPt = 0.0;

        const PrintPlan plan = buildPrintPlan(uniformPages(1), a4Printable(), options);
        QCOMPARE(plan.sheetCount(), 4);

        const SheetPlan& first = plan.sheets[0];
        const SheetPlan& second = plan.sheets[1];
        QCOMPARE(first.posterColumn, 0);
        QCOMPARE(second.posterColumn, 1);
        QCOMPARE(first.pageRectPt.topLeft(), QPointF(0.0, 0.0));
        // 第二欄把整頁往左推一張紙的寬度，裁切框固定貼在可列印區左上角。
        QCOMPARE(second.pageRectPt.left(), -kA4Width);
        QCOMPARE(second.clipRectPt.topLeft(), QPointF(0.0, 0.0));
        QVERIFY(second.clipRectPt.width() <= kA4Width + 1e-9);
        QVERIFY(qFuzzyCompare(first.pageRectPt.width(), second.pageRectPt.width()));
    }

    void batesIsConstantAcrossPosterSheetsOfTheSamePage() {
        PrintOptions options;
        options.poster.enabled = true;
        options.poster.zoomPercent = 200.0;
        options.poster.overlapPt = 0.0;
        options.stamps.bates.enabled = true;
        options.stamps.bates.digits = 4;

        const PrintPlan plan = buildPrintPlan(uniformPages(2), a4Printable(), options);
        QCOMPARE(plan.sheetCount(), 8);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(plan.sheets[static_cast<std::size_t>(i)].batesText, QStringLiteral("0001"));
        }
        for (int i = 4; i < 8; ++i) {
            QCOMPARE(plan.sheets[static_cast<std::size_t>(i)].batesText, QStringLiteral("0002"));
        }
    }

    void batesFollowsPrintedSubsetOnly() {
        PrintOptions options;
        options.pageRangeSpec = QStringLiteral("4-6");
        options.stamps.bates.enabled = true;
        options.stamps.bates.digits = 3;
        options.stamps.bates.startNumber = 10;

        const PrintPlan plan = buildPrintPlan(uniformPages(10), a4Printable(), options);
        QCOMPARE(plan.sheetCount(), 3);
        QCOMPARE(plan.sheets[0].batesText, QStringLiteral("010"));
        QCOMPARE(plan.sheets[2].batesText, QStringLiteral("012"));
    }

    void invalidRangeMakesThePlanInvalid() {
        PrintOptions options;
        options.pageRangeSpec = QStringLiteral("nope");
        const PrintPlan plan = buildPrintPlan(uniformPages(3), a4Printable(), options);
        QVERIFY(!plan.valid);
        QVERIFY(!plan.diagnostic.isEmpty());
    }

    void emptyPrintableAreaIsRejected() {
        PrintOptions options;
        const PrintPlan plan = buildPrintPlan(uniformPages(3), QRectF{}, options);
        QVERIFY(!plan.valid);
    }
};

QTEST_APPLESS_MAIN(TestPrintPlan)
#include "test_print_plan.moc"
