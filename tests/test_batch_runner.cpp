// 批次處理執行器（PRD-MAC-001，WP35）。
//
// 三件事是這支測試的核心，直接對應 app/batch_runner.h 開頭列的三條判準：
// dry-run 完全不碰檔案系統、輸出永遠是新檔案（原檔位元組不變）、
// 巨集裡的每個動作都真的執行了對應的既有服務（用可觀察的檔案變化驗證，
// 不是只驗證「呼叫沒有回傳錯誤」）。

#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <fstream>
#include <string>

#include "app/batch_runner.h"
#include "domain/macro.h"
#include "engine/bookmarks/outline_reader.h"
#include "engine/objects/pdf_source_document.h"

using namespace alioth;
using namespace alioth::app;

namespace {

std::string readFile(const QString& path) {
    std::ifstream file(path.toStdString(), std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool writeFile(const QString& path, const std::string& bytes) {
    std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

// 兩頁的最小 PDF，內容串流裡有一塊純紅色矩形供色彩轉換驗證。
std::string twoPageRedRectanglePdf() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>");
    const std::string content = "1 0 0 rg 0 0 50 50 re f\n";
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
                      "/Resources << >> >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                      "endstream");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 6 0 R "
                      "/Resources << >> >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                      "endstream");

    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
          " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

}  // namespace

class TestBatchRunner : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        pdf_ = twoPageRedRectanglePdf();
    }

    void formatOutputPathNeverEqualsInput() {
        const std::string out =
            BatchRunner::formatOutputPath("C:/docs/report.pdf", "{name}_batch{ext}");
        QVERIFY(out != "C:/docs/report.pdf");
        QVERIFY(out.find("report_batch.pdf") != std::string::npos);
    }

    void dryRunNeverTouchesFilesystem() {
        const QString inputPath = dir_->filePath(QStringLiteral("dry.pdf"));
        // 刻意不寫入檔案：dry-run 的合約是連「檔案存不存在」都不檢查。
        domain::macro::MacroDefinition macro;
        macro.name = "noop check";
        domain::macro::MacroAction rotate;
        rotate.kind = domain::macro::MacroActionKind::RotatePages;
        rotate.rotationDegrees = 90;
        macro.steps.push_back(rotate);

        BatchRunOptions options;
        options.dryRun = true;
        const BatchRunResult result =
            BatchRunner().run(macro, {inputPath.toStdString()}, options);
        QVERIFY(result.ok);
        QCOMPARE(result.files.size(), std::size_t{1});
        QVERIFY2(result.files[0].ok, result.files[0].diagnostic.c_str());
        QCOMPARE(result.files[0].stepDescriptions.size(), std::size_t{1});
        QVERIFY(!QFileInfo::exists(QString::fromStdString(result.files[0].outputPath)));
    }

    void invalidMacroFailsBeforeTouchingAnyFile() {
        domain::macro::MacroDefinition macro;
        macro.name = "bad";  // 沒有任何 step，validate() 應該失敗
        const BatchRunResult result = BatchRunner().run(macro, {"whatever.pdf"});
        QVERIFY(!result.ok);
        QVERIFY(result.files.empty());
    }

    void realRunAppliesRotateColorAndBarcodeWithoutTouchingOriginal() {
        const QString inputPath = dir_->filePath(QStringLiteral("input.pdf"));
        QVERIFY(writeFile(inputPath, pdf_));

        domain::macro::MacroDefinition macro;
        macro.name = "rotate + grayscale + barcode";

        domain::macro::MacroAction rotate;
        rotate.kind = domain::macro::MacroActionKind::RotatePages;
        rotate.rotationDegrees = 90;
        macro.steps.push_back(rotate);

        domain::macro::MacroAction color;
        color.kind = domain::macro::MacroActionKind::ConvertColor;
        color.colorSettings.mode = domain::enhance::ColorTransformMode::Grayscale;
        macro.steps.push_back(color);

        domain::macro::MacroAction barcode;
        barcode.kind = domain::macro::MacroActionKind::AddBarcodeStamp;
        barcode.barcodeText = "BATCH-1";
        barcode.barcodeRect = domain::RectF{60.0, 150.0, 190.0, 195.0};
        macro.steps.push_back(barcode);

        QVERIFY(macro.validate().empty());

        BatchRunOptions options;
        options.dryRun = false;
        options.outputPattern = "{name}_out{ext}";
        const BatchRunResult result =
            BatchRunner().run(macro, {inputPath.toStdString()}, options);
        QVERIFY(result.ok);
        QCOMPARE(result.files.size(), std::size_t{1});
        QVERIFY2(result.files[0].ok, result.files[0].diagnostic.c_str());

        const QString outputPath = QString::fromStdString(result.files[0].outputPath);
        QVERIFY(outputPath != inputPath);
        QVERIFY(QFileInfo::exists(outputPath));

        // 原檔必須一個位元組都沒動。
        const std::string originalAfter = readFile(inputPath);
        QCOMPARE(originalAfter, pdf_);

        const std::string produced = readFile(outputPath);
        QVERIFY(!produced.empty());
        QVERIFY(produced != pdf_);
        // 條碼文字應該以人讀文字的形式出現在輸出裡。
        QVERIFY2(produced.find("BATCH-1") != std::string::npos, "輸出裡找不到條碼的人讀文字");
        // 色彩轉換後純紅色矩形的內容運算子應該已經變成灰階（0.299 g），
        // 不再是原本的 "1 0 0 rg"。
        QVERIFY2(produced.find("1 0 0 rg") == std::string::npos,
                "色彩轉換沒有生效，原始的紅色運算子還在");
    }

    void outputPathCollisionIsRejectedExplicitly() {
        const QString inputPath = dir_->filePath(QStringLiteral("collide.pdf"));
        QVERIFY(writeFile(inputPath, pdf_));

        domain::macro::MacroDefinition macro;
        macro.name = "collide";
        domain::macro::MacroAction rotate;
        rotate.kind = domain::macro::MacroActionKind::RotatePages;
        macro.steps.push_back(rotate);

        BatchRunOptions options;
        options.dryRun = false;
        options.outputPattern = "{name}{ext}";  // 刻意與輸入同名
        const BatchRunResult result =
            BatchRunner().run(macro, {inputPath.toStdString()}, options);
        QVERIFY(result.ok);  // 巨集本身合法，只是這一個檔案的輸出路徑被拒絕
        QVERIFY(!result.files[0].ok);
        QVERIFY(result.files[0].diagnostic.find("相同") != std::string::npos);
        // 原檔仍然完好。
        QCOMPARE(readFile(inputPath), pdf_);
    }

    // PRD-BM-020：進階書籤巨集接進既有的 BatchRunner，這裡驗證的是「巨集
    // 真的把書籤寫進了輸出檔」，不是重複驗證 bookmark_ops 的樹狀邏輯本身
    // （那些在 test_bookmark_ops 已經測過）。
    void bookmarkMacroStepsProduceBookmarksInOutputFile() {
        const QString inputPath = dir_->filePath(QStringLiteral("bm-input.pdf"));
        QVERIFY(writeFile(inputPath, pdf_));

        domain::macro::MacroDefinition macro;
        macro.name = "every page bookmark + affix";

        domain::macro::MacroAction everyN;
        everyN.kind = domain::macro::MacroActionKind::BookmarkEveryNPages;
        everyN.bookmarkInterval = 1;
        everyN.bookmarkTitlePattern = "Page {page}";
        macro.steps.push_back(everyN);

        domain::macro::MacroAction affix;
        affix.kind = domain::macro::MacroActionKind::BookmarkAddAffix;
        affix.bookmarkAffix.prefix = "TOC: ";
        macro.steps.push_back(affix);

        QVERIFY2(macro.validate().empty(), macro.validate().c_str());

        BatchRunOptions options;
        options.dryRun = false;
        options.outputPattern = "{name}_bm{ext}";
        const BatchRunResult result =
            BatchRunner().run(macro, {inputPath.toStdString()}, options);
        QVERIFY(result.ok);
        QVERIFY2(result.files[0].ok, result.files[0].diagnostic.c_str());

        const std::string produced = readFile(QString::fromStdString(result.files[0].outputPath));
        QVERIFY(!produced.empty());

        engine::objects::PdfSourceDocument source;
        std::string openDiagnostic;
        QCOMPARE(source.open(produced, &openDiagnostic), engine::objects::SourceStatus::Ok);

        const engine::bookmarks::OutlineReadResult outline = engine::bookmarks::readOutline(source);
        QVERIFY2(outline.ok, outline.diagnostic.c_str());
        QCOMPARE(outline.tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(outline.tree[0].title), QStringLiteral("TOC: Page 1"));
        QCOMPARE(QString::fromStdString(outline.tree[1].title), QStringLiteral("TOC: Page 2"));
        QCOMPARE(outline.tree[0].target.destination.pageIndex, 0);
        QCOMPARE(outline.tree[1].target.destination.pageIndex, 1);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    std::string pdf_;
};

QTEST_MAIN(TestBatchRunner)
#include "test_batch_runner.moc"
