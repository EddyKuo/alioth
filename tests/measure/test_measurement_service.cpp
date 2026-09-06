// 量測應用層服務測試（PRD-ANN-024 / 025 / 026 / 027，WP25）。
//
// 服務本身是薄殼：計算委派給 domain/measurement.h，檔案讀寫委派給
// engine/objects 既有通道。這裡驗證的是「串起來的順序是對的」，
// 不重複驗證 domain 或 engine 層已經測過的計算細節。

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include "app/measurement_service.h"
#include "domain/csv.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/incremental_appender.h"
#include "object_fixture.h"

using namespace alioth::app;
using alioth::domain::Annotation;
using alioth::domain::LineEnding;
using alioth::domain::LineGeometry;
using alioth::domain::MeasureInfo;
using alioth::domain::PointF;
using alioth::domain::PolygonGeometry;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;
using alioth::test::writeBytesTo;

namespace {

// 準備一份帶著一則已量測 Line 註解的暫存檔，回傳它的物件編號，供
// clearMeasurementValue／readExistingScale 測試使用。
struct PreparedFile {
    QString path;
    int annotationObject{0};
};

PreparedFile prepareFileWithMeasuredLine(const QString& dir, MeasureInfo scale) {
    using namespace alioth::engine::objects;

    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    const SourceStatus opened = appender.open(source);
    Q_ASSERT(opened == SourceStatus::Ok);
    Q_UNUSED(opened);

    Annotation line{};
    line.geometry = LineGeometry{PointF{0, 0}, PointF{72, 0}, LineEnding::None, LineEnding::None};
    line.measure = std::move(scale);
    line.contents = "已計算的舊值";
    const AnnotationWriteResult written = writeAnnotation(appender, 0, line);
    Q_ASSERT(written.ok);
    const BuildResult built = appender.build();
    Q_ASSERT(built.ok);

    PreparedFile result{};
    result.path = dir + QStringLiteral("/measured.pdf");
    // 寫檔的呼叫不能放在 Q_ASSERT 裡：release 建置定義了 NDEBUG，整個運算式
    // 連同副作用一起被編掉，檔案根本沒產生，而失敗訊息會是遙遠的「找不到檔案」。
    const bool wrote = writeBytesTo(result.path, built.bytes);
    Q_ASSERT(wrote);
    Q_UNUSED(wrote);
    result.annotationObject = written.annotationObject;
    return result;
}

}  // namespace

class TestMeasurementService : public QObject {
    Q_OBJECT

private slots:
    void calibrateSucceedsForValidInput() {
        CalibrationRequest request{};
        request.pointA = PointF{0, 0};
        request.pointB = PointF{72, 0};
        request.realDistance = 2540.0;
        request.unit = alioth::domain::LengthUnit::Millimetre;

        const CalibrationOutcome outcome = MeasurementService::calibrate(request);
        QVERIFY(outcome.ok);
        QVERIFY(outcome.measure.isCalibrated());
        QCOMPARE(QString::fromStdString(outcome.measure.ratioLabel), QStringLiteral("1:100"));
    }

    void calibrateFailsWhenPointsCoincide() {
        CalibrationRequest request{};
        request.pointA = PointF{5, 5};
        request.pointB = PointF{5, 5};
        request.realDistance = 100.0;

        const CalibrationOutcome outcome = MeasurementService::calibrate(request);
        QVERIFY(!outcome.ok);
        QVERIFY(!outcome.message.isEmpty());
    }

    void applyMeasurementFillsContentsForLine() {
        Annotation line{};
        line.geometry = LineGeometry{PointF{0, 0}, PointF{3, 4}, LineEnding::None, LineEnding::None};

        MeasureInfo scale;
        scale.unitsPerPoint = 2.0;
        scale.unitLabel = "mm";
        scale.ratioLabel = "1:2";

        bool ok = false;
        QString message;
        const Annotation result = MeasurementService::applyMeasurement(line, scale, &ok, &message);
        QVERIFY(ok);
        QCOMPARE(QString::fromStdString(result.contents), QStringLiteral("10 mm"));
        QVERIFY(result.measure.has_value());
        QVERIFY(!message.isEmpty());
    }

    void applyMeasurementFailsWhenUncalibrated() {
        Annotation line{};
        line.geometry = LineGeometry{PointF{0, 0}, PointF{3, 4}, LineEnding::None, LineEnding::None};

        bool ok = true;
        QString message;
        const Annotation result =
            MeasurementService::applyMeasurement(line, MeasureInfo{}, &ok, &message);
        QVERIFY(!ok);
        QVERIFY(!message.isEmpty());
        // 未校正時原註解不動——不寫出一個看似合理的假數字。
        QVERIFY(result.contents.empty());
        QVERIFY(!result.measure.has_value());
    }

    void applyMeasurementRejectsUnsupportedGeometry() {
        Annotation shape{};
        shape.geometry = alioth::domain::ShapeGeometry{alioth::domain::ShapeKind::Square};
        MeasureInfo scale;
        scale.unitsPerPoint = 1.0;
        scale.unitLabel = "mm";

        bool ok = true;
        const Annotation result = MeasurementService::applyMeasurement(shape, scale, &ok);
        QVERIFY(!ok);
        QVERIFY(!result.measure.has_value());
    }

    void applyMeasurementWarnsOnSelfIntersectingPolygon() {
        Annotation polygon{};
        // 與 domain 層測試同一組自相交頂點（見 test_measurement_domain.cpp）。
        polygon.geometry =
            PolygonGeometry{{PointF{0, 0}, PointF{6, 4}, PointF{6, 0}, PointF{0, 2}}};
        MeasureInfo scale;
        scale.unitsPerPoint = 1.0;
        scale.unitLabel = "mm";

        bool ok = false;
        QString message;
        const Annotation result = MeasurementService::applyMeasurement(polygon, scale, &ok, &message);
        QVERIFY(ok);  // 仍然寫出確定性的數值……
        QVERIFY(message.contains(QStringLiteral("自相交")));  // ……但訊息必須可見，不能靜默帶過
        QVERIFY(QString::fromStdString(result.contents).contains(QStringLiteral("自相交")));
    }

    void clearMeasurementValueUpdatesFileOnDisk() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const PreparedFile prepared = prepareFileWithMeasuredLine(dir.path(), MeasureInfo{
                                                                                  "1:1", "in", 1.0});

        MeasurementService service;
        QString message;
        const bool ok =
            service.clearMeasurementValue(prepared.path, prepared.annotationObject, &message);
        QVERIFY2(ok, message.toUtf8().constData());

        // 讀回檔案，確認 /Contents 真的消失了（而不是只在記憶體裡改過）。
        const QString scaleMessage;
        const MeasureInfo stillCalibrated =
            MeasurementService::readExistingScale(prepared.path, 0, 0, nullptr);
        QVERIFY(stillCalibrated.isCalibrated());  // /Measure 沒有被清掉
    }

    void readExistingScaleFindsCalibratedAnnotation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const PreparedFile prepared =
            prepareFileWithMeasuredLine(dir.path(), MeasureInfo{"1:1000", "mm", 35.0});

        QString message;
        const MeasureInfo scale =
            MeasurementService::readExistingScale(prepared.path, 0, 0, &message);
        QVERIFY2(scale.isCalibrated(), message.toUtf8().constData());
        QCOMPARE(QString::fromStdString(scale.unitLabel), QStringLiteral("mm"));
    }

    void readExistingScaleReportsMissingAnnotation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/plain.pdf");
        QVERIFY(writeBytesTo(path, toStdString(makeFixturePdf())));

        QString message;
        const MeasureInfo scale = MeasurementService::readExistingScale(path, 0, 0, &message);
        QVERIFY(!scale.isCalibrated());
        QVERIFY(!message.isEmpty());
    }

    void exportCsvRoundTripsThroughDomainParser() {
        std::vector<MeasurementRecord> records;
        MeasurementRecord a{};
        a.pageIndex = 0;
        a.kind = MeasurementKind::Distance;
        a.annotationId = QStringLiteral("line-1, \"main\"");  // 刻意含逗號與引號
        a.value = 12.5;
        a.unitLabel = QStringLiteral("mm");
        a.ratioLabel = QStringLiteral("1:100");
        records.push_back(a);

        MeasurementRecord b{};
        b.pageIndex = 3;
        b.kind = MeasurementKind::Area;
        b.annotationId = QStringLiteral("poly-9");
        b.value = 42.75;
        b.unitLabel = QStringLiteral("mm2");
        b.ratioLabel = QStringLiteral("1:50");
        b.selfIntersecting = true;
        records.push_back(b);

        const QByteArray csv = MeasurementService::exportCsv(records);
        const std::string csvStd(csv.constData(), static_cast<std::size_t>(csv.size()));
        const std::vector<std::vector<std::string>> parsed = alioth::domain::parseCsvDocument(csvStd);

        // 表頭 + 兩筆資料。
        QCOMPARE(static_cast<int>(parsed.size()), 3);
        QCOMPARE(QString::fromStdString(parsed[0][0]), QStringLiteral("Page"));
        QCOMPARE(QString::fromStdString(parsed[1][2]), QStringLiteral("line-1, \"main\""));
        QCOMPARE(QString::fromStdString(parsed[1][3]), QStringLiteral("12.5"));
        QCOMPARE(QString::fromStdString(parsed[2][1]), QStringLiteral("Area"));
        QCOMPARE(QString::fromStdString(parsed[2][6]), QStringLiteral("true"));
    }
};

QTEST_MAIN(TestMeasurementService)
#include "test_measurement_service.moc"
