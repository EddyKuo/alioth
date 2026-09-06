// 量測註解的物件層寫入與讀回測試（PRD-ANN-014 / 025 / 026，WP25）。
//
// 沿用 tests/objects/test_annotation_objects.cpp 的驗證分路：用我們自己的
// 剖析器逐鍵檢查物件結構，再用 PDFium 重新開啟確認檔案本身仍然可讀，
// 最後（若環境裝了 qpdf）用獨立於 PDFium 的第二意見驗結構完整性。

#include <QtTest>

#include <cstring>
#include <string>

#include "engine/annotations/annotation_document.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/measure_reader.h"
#include "engine/objects/measurement_writer.h"
#include "object_fixture.h"
#include "qpdf_check.h"

using namespace alioth::engine::objects;
using alioth::domain::Annotation;
using alioth::domain::LengthUnit;
using alioth::domain::LineEnding;
using alioth::domain::LineGeometry;
using alioth::domain::MeasureInfo;
using alioth::domain::PointF;
using alioth::domain::PolygonGeometry;
using alioth::domain::PolyLineGeometry;
using alioth::test::describeQpdfFailure;
using alioth::test::makeFixturePdf;
using alioth::test::PdfFixtureOptions;
using alioth::test::qpdfSkipReason;
using alioth::test::runQpdfCheckOnBytes;
using alioth::test::QpdfStatus;
using alioth::test::toStdString;

namespace {

MeasureInfo makeScale(double unitsPerPoint, const char* unit, const char* ratio) {
    MeasureInfo info;
    info.unitsPerPoint = unitsPerPoint;
    info.unitLabel = unit;
    info.ratioLabel = ratio;
    return info;
}

Annotation makePolygon(const std::vector<PointF>& vertices) {
    Annotation annotation{};
    annotation.color = {0.0, 0.4, 0.0};
    annotation.interiorColor = alioth::domain::ColorRgb{0.6, 1.0, 0.6};
    annotation.opacity = 1.0;
    annotation.border.width = 1.0;
    annotation.geometry = PolygonGeometry{vertices};
    return annotation;
}

Annotation makePolyLine(const std::vector<PointF>& vertices) {
    Annotation annotation{};
    annotation.color = {0.0, 0.0, 0.6};
    annotation.border.width = 1.5;
    annotation.geometry = PolyLineGeometry{vertices, LineEnding::None, LineEnding::OpenArrow};
    return annotation;
}

Annotation makeLine(PointF start, PointF end) {
    Annotation annotation{};
    annotation.color = {0.6, 0.0, 0.0};
    annotation.border.width = 1.0;
    annotation.geometry = LineGeometry{start, end, LineEnding::None, LineEnding::None};
    return annotation;
}

PdfObject reopenObject(const std::string& bytes, int number) {
    IncrementalAppender appender;
    if (appender.open(bytes) != SourceStatus::Ok) return PdfObject{};
    return appender.source().object(number);
}

PdfObject entryOf(const PdfObject& object, const char* key) {
    const PdfDictionary* dict = object.asDictionary();
    if (dict == nullptr) return PdfObject{};
    const PdfObject* value = dict->find(key);
    return value == nullptr ? PdfObject{} : *value;
}

QString nameOf(const PdfObject& object, const char* key) {
    return QString::fromStdString(entryOf(object, key).asName());
}

bool hasKey(const PdfObject& object, const char* key) {
    const PdfDictionary* dict = object.asDictionary();
    return dict != nullptr && dict->has(key);
}

std::size_t arraySize(const PdfObject& object) {
    const PdfArray* array = object.asArray();
    return array == nullptr ? 0 : array->size();
}

PdfObject elementOf(const PdfObject& object, std::size_t index) {
    const PdfArray* array = object.asArray();
    if (array == nullptr || index >= array->size()) return PdfObject{};
    return array->at(index);
}

}  // namespace

class TestMeasureWriter : public QObject {
    Q_OBJECT

private slots:
    void polygonWritesVerticesAndMeasureWithArea() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation polygon = makePolygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        polygon.measure = makeScale(2.0, "mm", "1:2");

        const AnnotationWriteResult written = writeAnnotation(appender, 0, polygon);
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Subtype"), QStringLiteral("Polygon"));
        QCOMPARE(arraySize(entryOf(annot, "Vertices")), std::size_t{8});

        const PdfObject measure = entryOf(annot, "Measure");
        QVERIFY(measure.isDictionary());
        QCOMPARE(nameOf(measure, "Subtype"), QStringLiteral("RL"));

        const PdfObject xEntry = elementOf(entryOf(measure, "X"), 0);
        QCOMPARE(entryOf(xEntry, "C").asNumber(), 2.0);
        QCOMPARE(QString::fromStdString(entryOf(xEntry, "U").asName()), QStringLiteral("mm"));

        // 面積換算因子是距離因子的平方（2.0^2 = 4.0），不是同一個數字——
        // 這是量測面積最容易寫錯的一步。
        const PdfObject aEntry = elementOf(entryOf(measure, "A"), 0);
        QCOMPARE(entryOf(aEntry, "C").asNumber(), 4.0);

        // Polygon 屬於量測家族，不應該像一般幾何註解那樣帶 /Popup。
        QCOMPARE(written.popupObject, 0);
    }

    void polyLineWritesVerticesAndMeasureWithoutArea() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation polyline = makePolyLine({{0, 0}, {3, 0}, {3, 4}});
        polyline.measure = makeScale(1.0, "ft", "1:12");

        const AnnotationWriteResult written = writeAnnotation(appender, 0, polyline);
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Subtype"), QStringLiteral("PolyLine"));
        QCOMPARE(arraySize(entryOf(annot, "Vertices")), std::size_t{6});
        QCOMPARE(QString::fromStdString(elementOf(entryOf(annot, "LE"), 1).asName()),
                 QStringLiteral("OpenArrow"));

        const PdfObject measure = entryOf(annot, "Measure");
        QVERIFY(measure.isDictionary());
        QVERIFY(!hasKey(measure, "A"));  // 周長量測不需要面積換算因子
        QVERIFY(hasKey(measure, "D"));
    }

    void lineWritesMeasureWithoutArea() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation line = makeLine({0, 0}, {100, 0});
        line.measure = makeScale(0.5, "m", "1:1000");

        const AnnotationWriteResult written = writeAnnotation(appender, 0, line);
        QVERIFY(written.ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QVERIFY(hasKey(annot, "Measure"));
        QVERIFY(!hasKey(entryOf(annot, "Measure"), "A"));
        QCOMPARE(written.popupObject, 0);  // Line 是量測家族的既有慣例
    }

    void uncalibratedMeasureIsNotWritten() {
        // measure 有值但 unitsPerPoint <= 0：絕不能寫出一個看起來合理、
        // 實際上是假 1:1 比例的 /Measure。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation line = makeLine({0, 0}, {100, 0});
        line.measure = MeasureInfo{};  // 未校正

        const AnnotationWriteResult written = writeAnnotation(appender, 0, line);
        QVERIFY(written.ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QVERIFY(!hasKey(annot, "Measure"));
    }

    void measureRoundTripsThroughReadMeasure() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation polygon = makePolygon({{0, 0}, {20, 0}, {20, 10}, {0, 10}});
        const MeasureInfo original = makeScale(3.5, "cm", "1:35");
        polygon.measure = original;

        const AnnotationWriteResult written = writeAnnotation(appender, 0, polygon);
        QVERIFY(written.ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfObject annot = reopened.source().object(written.annotationObject);
        const MeasureReadResult read = readMeasure(reopened.source(), annot);
        QVERIFY(read.calibrated);
        QVERIFY(read.hasAreaFormat);
        QCOMPARE(QString::fromStdString(read.measure.unitLabel), QStringLiteral("cm"));
        QCOMPARE(QString::fromStdString(read.measure.ratioLabel), QStringLiteral("1:35"));
        QVERIFY(qAbs(read.measure.unitsPerPoint - original.unitsPerPoint) < 1e-9);
    }

    void differentPagesCanCarryDifferentScales() {
        // 跨頁不同比例尺：同一份文件裡，第 0 頁與第 1 頁的量測註解各自帶
        // 自己的 /Measure，讀回時不能互相污染。
        PdfFixtureOptions options;
        options.pageCount = 2;
        const std::string source = toStdString(makeFixturePdf(options));
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation lineOnPage0 = makeLine({0, 0}, {72, 0});
        lineOnPage0.measure = makeScale(1.0, "in", "1:1");
        const AnnotationWriteResult first = writeAnnotation(appender, 0, lineOnPage0);
        QVERIFY(first.ok);

        Annotation lineOnPage1 = makeLine({0, 0}, {72, 0});
        lineOnPage1.measure = makeScale(100.0, "mm", "1:100");
        const AnnotationWriteResult second = writeAnnotation(appender, 1, lineOnPage1);
        QVERIFY(second.ok);

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const MeasureReadResult readFirst =
            readMeasure(reopened.source(), reopened.source().object(first.annotationObject));
        const MeasureReadResult readSecond =
            readMeasure(reopened.source(), reopened.source().object(second.annotationObject));
        QVERIFY(readFirst.calibrated && readSecond.calibrated);
        QVERIFY(qAbs(readFirst.measure.unitsPerPoint - 1.0) < 1e-9);
        QVERIFY(qAbs(readSecond.measure.unitsPerPoint - 100.0) < 1e-9);
        QCOMPARE(QString::fromStdString(readFirst.measure.unitLabel), QStringLiteral("in"));
        QCOMPARE(QString::fromStdString(readSecond.measure.unitLabel), QStringLiteral("mm"));
    }

    void clearMeasurementValueRemovesContentsButKeepsMeasure() {
        // IncrementalAppender::updateObject 只覆寫「原檔已有」的物件（見其註解），
        // 剛在同一個 appender session 裡新配的物件屬於 pending，不算原檔物件。
        // 這與實際使用情境一致：MeasurementService::clearMeasurementValue 一律
        // 重新從磁碟開檔，因此這裡也先 build 再重新開一個 appender 模擬同樣的情境，
        // 而不是在同一個尚未落盤的 session 裡清除剛寫好的註解。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender writer;
        QCOMPARE(writer.open(source), SourceStatus::Ok);

        Annotation line = makeLine({0, 0}, {72, 0});
        line.measure = makeScale(1.0, "in", "1:1");
        line.contents = "1 in";
        const AnnotationWriteResult written = writeAnnotation(writer, 0, line);
        QVERIFY(written.ok);
        const BuildResult firstSave = writer.build();
        QVERIFY(firstSave.ok);

        IncrementalAppender appender;
        QCOMPARE(appender.open(firstSave.bytes), SourceStatus::Ok);
        const MeasurementClearResult cleared =
            clearMeasurementValue(appender, written.annotationObject);
        QVERIFY2(cleared.ok, cleared.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QVERIFY(!hasKey(annot, "Contents"));
        QVERIFY(hasKey(annot, "Measure"));  // 比例與幾何都還在
        QVERIFY(hasKey(annot, "L"));
    }

    void clearMeasurementValueFailsWithoutMeasure() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        // 一般螢光筆不是量測註解，清除量測值應明確拒絕，而不是靜默刪掉 /Contents。
        Annotation highlight{};
        highlight.geometry = alioth::domain::TextMarkupGeometry{
            alioth::domain::TextMarkupKind::Highlight,
            {alioth::domain::quadFromPageRect(alioth::domain::RectF{0, 0, 10, 10})}};
        highlight.contents = "備註";
        const AnnotationWriteResult written = writeAnnotation(appender, 0, highlight);
        QVERIFY(written.ok);

        const MeasurementClearResult cleared =
            clearMeasurementValue(appender, written.annotationObject);
        QVERIFY(!cleared.ok);
        QVERIFY(!cleared.diagnostic.empty());
    }

    void pdfiumReopensPolygonAndPolyLineAnnotations() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        QVERIFY(writeAnnotation(appender, 0, makePolygon({{0, 0}, {10, 0}, {10, 10}})).ok);
        QVERIFY(writeAnnotation(appender, 0, makePolyLine({{0, 0}, {5, 5}})).ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        alioth::engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(built.bytes.data(), built.bytes.size()));
        QCOMPARE(document.annotationCount(0), 2);
    }

    void producedFileStillPassesQpdfCheck() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation polygon = makePolygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        polygon.measure = makeScale(1.0, "mm", "1:1");
        QVERIFY(writeAnnotation(appender, 0, polygon).ok);

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const auto result = runQpdfCheckOnBytes(
            QStringLiteral("%1/alioth_measure_qpdf_check.pdf").arg(QDir::tempPath()),
            QByteArray(built.bytes.data(), static_cast<qsizetype>(built.bytes.size())));
        if (result.status == QpdfStatus::NotAvailable) {
            QSKIP(qpdfSkipReason().constData());
        }
        QVERIFY2(result.clean(), describeQpdfFailure(QStringLiteral("量測註解輸出"), result).constData());
    }
};

QTEST_APPLESS_MAIN(TestMeasureWriter)
#include "test_measure_writer.moc"
