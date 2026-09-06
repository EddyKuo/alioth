// /Measure 讀取的診斷測試（PRD-ANN-024，WP25）。
//
// 這裡刻意不經過真正的檔案 I/O 或 PDFium：readMeasure() 只需要一個已解出的
// 註解字典物件，用內嵌（非間接參照）的字典直接建構最快，也最容易把「格式
// 看不懂」的各種變形一一枚舉出來。跨物件的間接參照留給 test_measure_writer
// 的寫入/讀回整合測試覆蓋。

#include <QtTest>

#include "engine/objects/measure_reader.h"

using namespace alioth::engine::objects;

namespace {

PdfObject numberFormat(const std::string& unit, double factor) {
    PdfDictionary dict;
    dict.set("Type", makeName("NumberFormat"));
    dict.set("U", makeName(unit));
    dict.set("C", PdfObject{factor});
    return PdfObject{std::move(dict)};
}

PdfObject axisArray(const std::string& unit, double factor) {
    PdfArray array;
    array.push_back(numberFormat(unit, factor));
    return PdfObject{std::move(array)};
}

}  // namespace

class TestMeasureReader : public QObject {
    Q_OBJECT

private slots:
    void missingMeasureKeyReportsUncalibrated() {
        PdfDictionary annot;
        annot.set("Type", makeName("Annot"));
        annot.set("Subtype", makeName("Line"));

        const PdfSourceDocument source;  // 不需要任何真實檔案：/Measure 是內嵌字典
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(!result.calibrated);
        QVERIFY(!result.diagnostic.empty());
    }

    void nonDictionaryAnnotationIsRejected() {
        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{static_cast<std::int64_t>(1)});
        QVERIFY(!result.calibrated);
        QVERIFY(!result.diagnostic.empty());
    }

    void unsupportedMeasureSubtypeIsRejected() {
        PdfDictionary measure;
        measure.set("Type", makeName("Measure"));
        measure.set("Subtype", makeName("XY"));  // ISO 32000-1 只定義了 RL
        measure.set("X", axisArray("mm", 1.0));

        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(!result.calibrated);
        QVERIFY(QString::fromStdString(result.diagnostic).contains(QStringLiteral("Subtype")));
    }

    void wellFormedMeasureIsCalibrated() {
        PdfDictionary measure;
        measure.set("Type", makeName("Measure"));
        measure.set("Subtype", makeName("RL"));
        measure.set("R", makeLiteralString("1:100"));
        measure.set("X", axisArray("mm", 35.277777));
        measure.set("D", axisArray("mm", 35.277777));

        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(result.calibrated);
        QCOMPARE(QString::fromStdString(result.measure.unitLabel), QStringLiteral("mm"));
        QVERIFY(qAbs(result.measure.unitsPerPoint - 35.277777) < 1e-6);
        QCOMPARE(QString::fromStdString(result.measure.ratioLabel), QStringLiteral("1:100"));
        QVERIFY(!result.hasAreaFormat);
    }

    void areaFormatIsReportedWhenPresent() {
        PdfDictionary measure;
        measure.set("Subtype", makeName("RL"));
        measure.set("X", axisArray("mm", 1.0));
        measure.set("A", axisArray("mm2", 1.0));

        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(result.calibrated);
        QVERIFY(result.hasAreaFormat);
    }

    // 非等向縮放：X 與 Y 兩軸的換算因子明顯不同。CLAUDE.md 的要求是明確拒絕，
    // 不能挑一軸將就——那會產生看起來合理、實際上系統性錯誤的數字。
    void nonUniformXAndYIsRejected() {
        PdfDictionary measure;
        measure.set("Subtype", makeName("RL"));
        measure.set("X", axisArray("mm", 1.0));
        measure.set("Y", axisArray("mm", 1.3));  // 與 X 軸差 30%

        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(!result.calibrated);
        QVERIFY(QString::fromStdString(result.diagnostic).contains(QStringLiteral("非等向")));
    }

    void nearlyEqualXAndYIsAcceptedAsUniform() {
        // 浮點捨入造成的微小差異（例如兩個軸各自算出 35.277776 與 35.277778）
        // 不該被誤判成非等向縮放。
        PdfDictionary measure;
        measure.set("Subtype", makeName("RL"));
        measure.set("X", axisArray("mm", 35.277776));
        measure.set("Y", axisArray("mm", 35.277778));

        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(result.calibrated);
    }

    void missingXAxisIsRejected() {
        PdfDictionary measure;
        measure.set("Subtype", makeName("RL"));
        // 沒有 /X：格式不完整，不能猜一個係數出來。
        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(!result.calibrated);
    }

    void zeroOrNegativeFactorIsRejected() {
        PdfDictionary measure;
        measure.set("Subtype", makeName("RL"));
        measure.set("X", axisArray("mm", 0.0));

        PdfDictionary annot;
        annot.set("Measure", PdfObject{std::move(measure)});

        const PdfSourceDocument source;
        const MeasureReadResult result = readMeasure(source, PdfObject{std::move(annot)});
        QVERIFY(!result.calibrated);
    }
};

QTEST_APPLESS_MAIN(TestMeasureReader)
#include "test_measure_reader.moc"
