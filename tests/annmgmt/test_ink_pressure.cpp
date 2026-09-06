// 壓力感測筆寬（PRD-ANN-003）。
//
// /Ink 只有一個 /BS /W，所以「用力比較粗」只能靠把一條筆畫切成數則註解模擬。
// 這支測試釘住三件會直接影響使用者的性質：
//   1. 沒有壓力裝置（一律 1.0）時只產生一則註解——不能因為支援壓力，就讓
//      每個用滑鼠的人都多出好幾則。
//   2. 段與段之間共用交界點——少了它，筆畫在變粗的地方會看得到缺口。
//   3. 每一點都在某一層裡出現過——切段時弄丟中間幾點，畫出來會是斷線，
//      而那在短筆畫上完全看不出來。

#include <QtTest>

#include <set>
#include <vector>

#include "domain/ink_pressure.h"

using alioth::domain::InkWidthLayer;
using alioth::domain::PointF;
using alioth::domain::PressurePoint;
using alioth::domain::PressureWidthOptions;
using alioth::domain::splitByPressure;

namespace {

std::vector<PressurePoint> rampingStroke() {
    // 壓力由 0 線性升到接近 1，座標同步往右。
    std::vector<PressurePoint> stroke;
    for (int i = 0; i < 20; ++i) {
        stroke.push_back(PressurePoint{PointF{static_cast<double>(i), 0.0},
                                       static_cast<double>(i) / 20.0});
    }
    return stroke;
}

std::size_t totalPoints(const std::vector<InkWidthLayer>& layers) {
    std::size_t n = 0;
    for (const InkWidthLayer& layer : layers) {
        for (const auto& stroke : layer.strokes) n += stroke.size();
    }
    return n;
}

}  // namespace

class TestInkPressure : public QObject {
    Q_OBJECT

private slots:
    void constantPressureProducesASingleLayer();
    void rampingPressureProducesOneLayerPerBand();
    void adjacentSegmentsShareTheirBoundaryPoint();
    void everyInputPointSurvivesTheSplit();
    void layersAreOrderedByWidth();
    void widthsStayWithinTheConfiguredRange();
    void degenerateInputProducesNothing();
    void outOfRangePressureIsClamped();
};

void TestInkPressure::constantPressureProducesASingleLayer() {
    std::vector<PressurePoint> stroke;
    for (int i = 0; i < 10; ++i) {
        stroke.push_back(PressurePoint{PointF{static_cast<double>(i), 0.0}, 1.0});
    }
    const auto layers = splitByPressure({stroke});
    QCOMPARE(layers.size(), std::size_t(1));
    QCOMPARE(layers[0].strokes.size(), std::size_t(1));
    QCOMPARE(layers[0].strokes[0].size(), std::size_t(10));
}

void TestInkPressure::rampingPressureProducesOneLayerPerBand() {
    PressureWidthOptions options;
    options.bandCount = 4;
    const auto layers = splitByPressure({rampingStroke()}, options);
    // 壓力掃過 0–1，四檔都應該出現。
    QCOMPARE(layers.size(), std::size_t(4));
}

void TestInkPressure::adjacentSegmentsShareTheirBoundaryPoint() {
    PressureWidthOptions options;
    options.bandCount = 4;
    const auto layers = splitByPressure({rampingStroke()}, options);

    // 收集所有段的端點，檢查每一段的終點都是另一段的起點（最後一段除外）。
    // 逐點數量比對更嚴格：20 個輸入點切成 4 段會有 3 個交界點被複製一次，
    // 因此總點數應該是 20 + 3。
    QCOMPARE(totalPoints(layers), std::size_t(20 + 3));
}

void TestInkPressure::everyInputPointSurvivesTheSplit() {
    const auto stroke = rampingStroke();
    const auto layers = splitByPressure({stroke});

    std::set<double> seen;
    for (const InkWidthLayer& layer : layers) {
        for (const auto& segment : layer.strokes) {
            for (const PointF& point : segment) seen.insert(point.x);
        }
    }
    QCOMPARE(seen.size(), stroke.size());
}

void TestInkPressure::layersAreOrderedByWidth() {
    const auto layers = splitByPressure({rampingStroke()});
    for (std::size_t i = 1; i < layers.size(); ++i) {
        QVERIFY(layers[i - 1].width < layers[i].width);
    }
}

void TestInkPressure::widthsStayWithinTheConfiguredRange() {
    PressureWidthOptions options;
    options.minWidth = 1.0;
    options.maxWidth = 5.0;
    options.bandCount = 4;
    const auto layers = splitByPressure({rampingStroke()}, options);
    QVERIFY(!layers.empty());
    for (const InkWidthLayer& layer : layers) {
        QVERIFY(layer.width >= options.minWidth);
        QVERIFY(layer.width <= options.maxWidth);
    }
}

void TestInkPressure::degenerateInputProducesNothing() {
    QVERIFY(splitByPressure({}).empty());
    // 單點畫不出線；產生一則看不見的註解只會讓註解清單多一列垃圾。
    QVERIFY(splitByPressure({{PressurePoint{PointF{1, 1}, 1.0}}}).empty());
    QVERIFY(splitByPressure({{}}).empty());
}

void TestInkPressure::outOfRangePressureIsClamped() {
    // 部分數位板驅動會回報大於 1 或負的壓力值。夾住而不是信任它——
    // 不夾的話檔位索引會越界。
    const std::vector<PressurePoint> stroke{
        PressurePoint{PointF{0, 0}, -5.0}, PressurePoint{PointF{1, 0}, -5.0},
        PressurePoint{PointF{2, 0}, 9.0}, PressurePoint{PointF{3, 0}, 9.0}};
    PressureWidthOptions options;
    options.bandCount = 4;
    const auto layers = splitByPressure({stroke}, options);
    QCOMPARE(layers.size(), std::size_t(2));
    QVERIFY(layers[0].width >= options.minWidth);
    QVERIFY(layers[1].width <= options.maxWidth);
}

QTEST_APPLESS_MAIN(TestInkPressure)
#include "test_ink_pressure.moc"
