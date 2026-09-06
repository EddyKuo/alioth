// 量測領域模型的純函數測試（PRD-ANN-014 / 024，WP25）。
//
// 不連結任何引擎或 Qt 事件迴圈相依：比例換算與幾何計算是全案對正確性要求
// 最嚴的一段邏輯，必須在毫秒級跑完，邊界情況才有機會被密集覆蓋。

#include <QtTest>

#include "domain/csv.h"
#include "domain/measurement.h"

using namespace alioth::domain;

namespace {

constexpr double kEpsilon = 1e-9;

}  // namespace

class TestMeasurementDomain : public QObject {
    Q_OBJECT

private slots:
    // ---- 校正 ----

    void calibrateRejectsNonPositivePaperDistance() {
        const CalibrationResult result = calibrateUniform(0.0, 10.0, LengthUnit::Millimetre);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(!result.measure.isCalibrated());
    }

    void calibrateRejectsNonPositiveRealDistance() {
        const CalibrationResult result = calibrateUniform(72.0, -5.0, LengthUnit::Millimetre);
        QVERIFY(!result.ok);
        QVERIFY(!result.measure.isCalibrated());
    }

    void calibrateRejectsNonFiniteInputs() {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!calibrateUniform(nan, 10.0, LengthUnit::Millimetre).ok);
        QVERIFY(!calibrateUniform(72.0, nan, LengthUnit::Millimetre).ok);
    }

    void calibrateComputesRatioAndUnitsPerPoint() {
        // 72 點 = 1 吋的紙面距離；1 吋在紙面上實際印出來是 25.4 公釐。
        // 真實距離設成 2540 公釐，比例應為 1:100。
        const CalibrationResult result = calibrateUniform(72.0, 2540.0, LengthUnit::Millimetre);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(QString::fromStdString(result.measure.ratioLabel), QStringLiteral("1:100"));
        QCOMPARE(QString::fromStdString(result.measure.unitLabel), QStringLiteral("mm"));
        QVERIFY(qAbs(result.measure.unitsPerPoint - (2540.0 / 72.0)) < kEpsilon);
        QVERIFY(result.measure.isCalibrated());
    }

    void uncalibratedMeasureInfoReportsFalse() {
        MeasureInfo info;
        QVERIFY(!info.isCalibrated());
        info.unitsPerPoint = -1.0;
        QVERIFY(!info.isCalibrated());
    }

    // ---- 距離／周長 ----

    void distanceRequiresCalibration() {
        const MeasurementResult result = measureDistance(PointF{0, 0}, PointF{10, 0}, MeasureInfo{});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void distanceAppliesScale() {
        MeasureInfo scale;
        scale.unitsPerPoint = 2.0;
        scale.unitLabel = "mm";
        const MeasurementResult result = measureDistance(PointF{0, 0}, PointF{3, 4}, scale);
        QVERIFY(result.ok);
        // 3-4-5 直角三角形斜邊 = 5 點，換算後 10 mm。
        QVERIFY(qAbs(result.value - 10.0) < kEpsilon);
        QCOMPARE(QString::fromStdString(result.unitLabel), QStringLiteral("mm"));
    }

    void distanceHandlesVeryTinySeparation() {
        // 極小距離：確保沒有因為浮點下溢或提前判定為零而給出錯誤的零值。
        MeasureInfo scale;
        scale.unitsPerPoint = 1000.0;
        const MeasurementResult result =
            measureDistance(PointF{0, 0}, PointF{1e-6, 0}, scale);
        QVERIFY(result.ok);
        QVERIFY(result.value > 0.0);
        QVERIFY(qAbs(result.value - 1e-3) < 1e-9);
    }

    void polylineLengthSumsSegmentsWithoutClosing() {
        const std::vector<PointF> path = {{0, 0}, {3, 0}, {3, 4}};
        // 3 + 4 = 7；刻意不加回起點的邊。
        QVERIFY(qAbs(polylineLength(path) - 7.0) < kEpsilon);
    }

    void polygonPerimeterClosesThePath() {
        const std::vector<PointF> path = {{0, 0}, {3, 0}, {3, 4}};
        // 折線總長 7，加上回到起點的閉合邊（長度 5）＝ 12。
        QVERIFY(qAbs(polygonPerimeter(path) - 12.0) < kEpsilon);
    }

    void perimeterMeasurementRequiresCalibration() {
        const std::vector<PointF> path = {{0, 0}, {3, 0}, {3, 4}};
        const MeasurementResult result = measurePerimeter(path, MeasureInfo{}, /*closed=*/false);
        QVERIFY(!result.ok);
    }

    // ---- 面積 ----

    void polygonAreaOfASquare() {
        const std::vector<PointF> square = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        const AreaResult result = computePolygonArea(square);
        QVERIFY(qAbs(result.area - 100.0) < kEpsilon);
        QVERIFY(!result.selfIntersecting);
    }

    void polygonAreaAppliesSquaredScale() {
        const std::vector<PointF> square = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        MeasureInfo scale;
        scale.unitsPerPoint = 2.0;  // 面積換算要是 2^2 = 4 倍，不是 2 倍。
        scale.unitLabel = "mm";
        const MeasurementResult result = measureArea(square, scale);
        QVERIFY(result.ok);
        QVERIFY(qAbs(result.value - 400.0) < kEpsilon);
        QCOMPARE(QString::fromStdString(result.unitLabel), QStringLiteral("mm2"));
    }

    void areaMeasurementRequiresCalibration() {
        const std::vector<PointF> square = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        QVERIFY(!measureArea(square, MeasureInfo{}).ok);
    }

    void tooFewVerticesYieldsZeroArea() {
        const std::vector<PointF> line = {{0, 0}, {10, 0}};
        const AreaResult result = computePolygonArea(line);
        QCOMPARE(result.area, 0.0);
        QVERIFY(!result.selfIntersecting);
    }

    // 自相交多邊形：CLAUDE.md 要求明確定義行為並測試，不能靜默給錯值。
    // 這是把一個簡單四邊形的兩個對角頂點互換順序得到的「蝴蝶結」，
    // 面積數字是 shoelace 帶號面積的絕對值——不是視覺上圈起來的面積，
    // 這裡把期望值釘死，確保實作不會悄悄換一種定義。
    void selfIntersectingPolygonIsDetectedAndAreaIsDefinedButNotVisualArea() {
        const std::vector<PointF> bowtie = {{0, 0}, {6, 4}, {6, 0}, {0, 2}};
        const AreaResult result = computePolygonArea(bowtie);
        QVERIFY(result.selfIntersecting);
        QVERIFY(qAbs(result.area - 6.0) < kEpsilon);

        // 面積量測套用比例後，selfIntersecting 旗標必須一路傳到最終結果，
        // 呼叫端才有機會提示使用者，而不是把這個數字當成可信賴的面積顯示。
        MeasureInfo scale;
        scale.unitsPerPoint = 1.0;
        scale.unitLabel = "mm";
        const MeasurementResult measured = measureArea(bowtie, scale);
        QVERIFY(measured.ok);
        QVERIFY(measured.selfIntersecting);
    }

    void simpleQuadrilateralIsNotFlaggedSelfIntersecting() {
        // 同樣四個頂點、依凸包順序連接則不自相交，作為上一個測試的對照組。
        const std::vector<PointF> simple = {{0, 0}, {6, 0}, {6, 4}, {0, 2}};
        QVERIFY(!isPolygonSelfIntersecting(simple));
    }

    void triangleCanNeverSelfIntersect() {
        const std::vector<PointF> triangle = {{0, 0}, {5, 0}, {0, 5}};
        QVERIFY(!isPolygonSelfIntersecting(triangle));
    }

    // ---- 非等向縮放 ----

    void resolveUniformScaleAcceptsMatchingAxes() {
        const auto resolved = resolveUniformScale(2.0, 2.0000001);
        QVERIFY(resolved.has_value());
        QVERIFY(qAbs(*resolved - 2.0) < 1e-6);
    }

    void resolveUniformScaleRejectsDivergentAxes() {
        // X 軸與 Y 軸差了 20%：非等向縮放，沒有單一正確係數，必須拒絕而不是
        // 挑一軸將就——挑錯軸算出來的距離／面積會是看起來合理的錯誤數字。
        const auto resolved = resolveUniformScale(1.0, 1.2);
        QVERIFY(!resolved.has_value());
    }

    void resolveUniformScaleTreatsMissingYAsUniform() {
        // /Measure 沒有 /Y 是常態（多數文件只給 /X），不能因此判成非等向。
        const auto resolved = resolveUniformScale(3.0, 0.0);
        QVERIFY(resolved.has_value());
        QCOMPARE(*resolved, 3.0);
    }

    // ---- 跨頁不同比例尺：MeasureInfo 彼此獨立 ----

    void measureInfoInstancesAreIndependentAcrossScales() {
        const CalibrationResult page1 = calibrateUniform(72.0, 2540.0, LengthUnit::Millimetre);
        const CalibrationResult page2 = calibrateUniform(36.0, 100.0, LengthUnit::Inch);
        QVERIFY(page1.ok);
        QVERIFY(page2.ok);
        QVERIFY(page1.measure.unitsPerPoint != page2.measure.unitsPerPoint);

        const MeasurementResult d1 = measureDistance(PointF{0, 0}, PointF{72, 0}, page1.measure);
        const MeasurementResult d2 = measureDistance(PointF{0, 0}, PointF{72, 0}, page2.measure);
        QVERIFY(d1.ok && d2.ok);
        QVERIFY(qAbs(d1.value - d2.value) > 1e-6);
    }

    // ---- CSV ----

    void csvEscapesCommaQuoteAndNewline() {
        QCOMPARE(QString::fromStdString(csvEscapeField("plain")), QStringLiteral("plain"));
        QCOMPARE(QString::fromStdString(csvEscapeField("a,b")), QStringLiteral("\"a,b\""));
        QCOMPARE(QString::fromStdString(csvEscapeField("a\"b")), QStringLiteral("\"a\"\"b\""));
        QCOMPARE(QString::fromStdString(csvEscapeField("a\nb")), QStringLiteral("\"a\nb\""));
        QCOMPARE(QString::fromStdString(csvEscapeField("a\rb")), QStringLiteral("\"a\rb\""));
    }

    void csvRoundTripsTrickyFields() {
        const std::vector<std::vector<std::string>> rows = {
            {"Page", "Type", "Value", "Note"},
            {"1", "Distance", "12.5", "含逗號, 與\"引號\""},
            {"2", "Area", "3.14", "多行\n備註"},
            {"3", "Perimeter", "0", ""},
        };
        const std::string document = csvDocument(rows);
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument(document);
        QCOMPARE(static_cast<int>(parsed.size()), static_cast<int>(rows.size()));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            QCOMPARE(static_cast<int>(parsed[i].size()), static_cast<int>(rows[i].size()));
            for (std::size_t j = 0; j < rows[i].size(); ++j) {
                QCOMPARE(QString::fromStdString(parsed[i][j]), QString::fromStdString(rows[i][j]));
            }
        }
    }

    void csvParsesDocumentWithoutTrailingNewline() {
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument("a,b,c");
        QCOMPARE(static_cast<int>(parsed.size()), 1);
        QCOMPARE(static_cast<int>(parsed.front().size()), 3);
        QCOMPARE(QString::fromStdString(parsed.front()[2]), QStringLiteral("c"));
    }

    void csvParsesEmptyDocumentAsNoRows() {
        QVERIFY(parseCsvDocument("").empty());
    }
};

QTEST_APPLESS_MAIN(TestMeasurementDomain)
#include "test_measurement_domain.moc"
