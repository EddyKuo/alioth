// CSV 灌入表單（PRD-FORM-023）。
//
// 錯誤處理是這個功能的重點，因此測試涵蓋：表頭欄名對不上範本欄位（整批的
// 結構問題，忽略該欄但不中止）、資料列欄數與表頭不符時 SkipRow 與
// AbortBatch 兩種政策的差異、以及成功路徑產出的檔案真的能被 PDFium 讀回。

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <atomic>

#include "app/csv_form_fill.h"
#include "engine/forms/form_document.h"
#include "form_pdf_fixture.h"

using namespace alioth;
using namespace alioth::app;
using namespace alioth::engine::forms;

namespace {

// 等待 runCsvFormFill 的 onDone 回呼。回呼經過 QMetaObject::invokeMethod
// 排回呼叫端（本測試的主執行緒）才會被呼叫（見 app/csv_form_fill.h 的
// 說明），因此這裡用忙等搭配 QTest::qWait 驅動事件迴圈，而不是假設它
// 同步完成。
// 失敗時把整份報告攤出來。這支測試在 ctest -j 下偶發失敗過一次而單獨跑會過，
// 當時的輸出只有「Failed」，完全沒有線索。斷言帶上診斷之後，下一次偶發
// 至少會留下可以追的東西——偶發而查不出原因的測試，最後都會被當成雜訊忽略。
QByteArray describeReport(const CsvFormFillReport& report) {
    QString text = QStringLiteral("aborted=%1 reason=%2 rows=%3 ok=%4 failed=%5")
                       .arg(report.aborted)
                       .arg(QString::fromStdString(report.abortReason))
                       .arg(report.rows.size())
                       .arg(report.succeededCount())
                       .arg(report.failedCount());
    for (std::size_t i = 0; i < report.rows.size(); ++i) {
        text += QStringLiteral("\n  第 %1 列 ok=%2 error=%3")
                    .arg(i)
                    .arg(report.rows[i].ok)
                    .arg(QString::fromStdString(report.rows[i].error));
    }
    for (const std::string& column : report.ignoredColumns) {
        text += QStringLiteral("\n  忽略欄 %1").arg(QString::fromStdString(column));
    }
    return text.toUtf8();
}

CsvFormFillReport runAndWait(const CsvFormFillOptions& options) {
    // 狀態放在 shared_ptr 而不是堆疊上：逾時後這個函式會返回，而回呼可能
    // 之後才被觸發。捕捉堆疊變數的參照在那一刻就是寫進已經死掉的記憶體，
    // 症狀會出現在**後面某個不相干的測試**上，幾乎追不回來。
    struct State {
        std::atomic<bool> done{false};
        CsvFormFillReport report;
    };
    const auto state = std::make_shared<State>();

    runCsvFormFill(options, [state](CsvFormFillReport r) {
        state->report = std::move(r);
        state->done = true;
    });

    // 預算放寬到 60 秒：整批要反覆開關 PDFium 文件把手，而 ctest 平行執行時
    // 機器上可能同時有八個同樣吃重的測試行程。原本的 10 秒在單獨執行時綽綽有餘，
    // 平行執行時卻會逾時——那種只在平行下失敗的測試，遲早會在 CI 隨機紅一次，
    // 然後被當成雜訊忽略。ctest 自己的 TIMEOUT 是 120 秒，仍然擋得住真正的當掉。
    QElapsedTimer timer;
    timer.start();
    while (!state->done.load() && timer.elapsed() < 60000) QTest::qWait(20);
    return state->report;
}

}  // namespace

class TestCsvFormFill : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        templatePath_ = dir_->filePath(QStringLiteral("template.pdf"));
        QFile file(templatePath_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = alioth::test::makeAcroFormPdf();
        file.write(bytes);
        file.close();

        outputDir_ = dir_->filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(outputDir_));
    }

    void unmatchedColumnIsIgnoredNotAborted() {
        CsvFormFillOptions options;
        options.templatePath = templatePath_.toStdString();
        options.outputDirectory = outputDir_.toStdString();
        options.csvText = "fullName,notAField\r\nBob,whatever\r\n";

        const CsvFormFillReport report = runAndWait(options);
        QVERIFY2(!report.aborted, report.abortReason.c_str());
        QCOMPARE(report.ignoredColumns.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(report.ignoredColumns.front()), QStringLiteral("notAField"));
        QVERIFY2(report.succeededCount() == 1, describeReport(report).constData());
    }

    void allColumnsUnmatchedAbortsBatch() {
        CsvFormFillOptions options;
        options.templatePath = templatePath_.toStdString();
        options.outputDirectory = outputDir_.toStdString();
        options.csvText = "notAField1,notAField2\r\nx,y\r\n";

        const CsvFormFillReport report = runAndWait(options);
        QVERIFY(report.aborted);
        QVERIFY(!report.abortReason.empty());
        QVERIFY(report.rows.empty());
    }

    void rowWithWrongColumnCountIsSkippedUnderSkipRowPolicy() {
        CsvFormFillOptions options;
        options.templatePath = templatePath_.toStdString();
        options.outputDirectory = outputDir_.toStdString();
        options.mismatchPolicy = CsvMismatchPolicy::SkipRow;
        // 第二列少一欄。
        options.csvText = "fullName,agree\r\nAlice,Yes\r\nBob\r\nCarol,Off\r\n";

        const CsvFormFillReport report = runAndWait(options);
        QVERIFY2(!report.aborted, report.abortReason.c_str());
        QVERIFY2(report.rows.size() == 3, describeReport(report).constData());
        QVERIFY2(report.rows[0].ok, describeReport(report).constData());
        QVERIFY(!report.rows[1].ok);
        QVERIFY(!report.rows[1].error.empty());
        QVERIFY(report.rows[2].ok);  // 政策是跳過，不是整批中止，第三列仍要處理
        QVERIFY2(report.succeededCount() == 2, describeReport(report).constData());
        QCOMPARE(report.failedCount(), std::int32_t{1});
    }

    void rowWithWrongColumnCountAbortsUnderAbortBatchPolicy() {
        CsvFormFillOptions options;
        options.templatePath = templatePath_.toStdString();
        options.outputDirectory = outputDir_.toStdString();
        options.mismatchPolicy = CsvMismatchPolicy::AbortBatch;
        options.csvText = "fullName,agree\r\nAlice,Yes\r\nBob\r\nCarol,Off\r\n";

        const CsvFormFillReport report = runAndWait(options);
        QVERIFY(report.aborted);
        // 中止之前已經處理過的列（第一列）仍然算數,但第三列完全沒被嘗試——
        // 兩者語意不同,呼叫端不能把「中止後未嘗試」誤算成「失敗」。
        QCOMPARE(report.rows.size(), std::size_t{1});
        QVERIFY(report.rows.front().ok);
    }

    void successfulRowProducesReadablePdf() {
        CsvFormFillOptions options;
        options.templatePath = templatePath_.toStdString();
        options.outputDirectory = outputDir_.toStdString();
        options.outputNamePattern = "row_{row}.pdf";
        options.csvText = "fullName\r\nZoe\r\n";

        const CsvFormFillReport report = runAndWait(options);
        QVERIFY2(!report.aborted, report.abortReason.c_str());
        QCOMPARE(report.succeededCount(), std::int32_t{1});
        const QString outputPath = QString::fromStdString(report.rows.front().outputPath);
        QVERIFY(QFile::exists(outputPath));

        FormDocument document;
        std::atomic<int> error{-1};
        document.open(outputPath.toStdString(), "",
                      [&error](domain::DocumentError e) { error = static_cast<int>(e); });
        document.waitForIdle();
        QCOMPARE(error.load(), static_cast<int>(domain::DocumentError::None));

        std::string value;
        document.allFields([&value](const std::vector<FormFieldInfo>& fields) {
            for (const FormFieldInfo& f : fields) {
                if (f.name == "fullName") value = f.value;
            }
        });
        document.waitForIdle();
        QCOMPARE(QString::fromStdString(value), QStringLiteral("Zoe"));
    }

    void emptyCsvAborts() {
        CsvFormFillOptions options;
        options.templatePath = templatePath_.toStdString();
        options.outputDirectory = outputDir_.toStdString();
        options.csvText = "";

        const CsvFormFillReport report = runAndWait(options);
        QVERIFY(report.aborted);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString templatePath_;
    QString outputDir_;
};

QTEST_MAIN(TestCsvFormFill)
#include "test_csv_form_fill.moc"
