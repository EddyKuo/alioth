// 擷取／合併／分割測試（WBS 5.7，PRD-PAGE-002）。
//
// 三者的共同驗收條件是「輸出檔可以被重新開啟，而且頁面順序是使用者要的那個順序」，
// 所以一律以文字標記比對，不以頁數比對。

#include <QtTest>

#include <string>
#include <vector>

#include "domain/page_operations.h"
#include "engine/pages/page_editor.h"
#include "page_fixture.h"

using namespace alioth::domain::pages;
using namespace alioth::engine::pages;

namespace {

std::vector<std::string> expectMarkers(std::initializer_list<int> oneBasedPages) {
    std::vector<std::string> out;
    for (const int page : oneBasedPages) {
        out.push_back(alioth::test::pageMarker(page).toStdString());
    }
    return out;
}

}  // namespace

class TestPageAssembly : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        source_ = dir_->filePath(QStringLiteral("source.pdf"));
        QVERIFY(alioth::test::writePdfTo(source_, alioth::test::makeMarkedPdf(6)));
    }

    void cleanup() { dir_.reset(); }

    // ---- 擷取 -------------------------------------------------------------

    void extractsOnlySelectedPagesInGivenOrder() {
        const QString target = dir_->filePath(QStringLiteral("extract.pdf"));
        const AssemblyResult result =
            extractPages(source_.toStdString(), {4, 0, 2}, target.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.pageCount, 3);
        QVERIFY(result.save.ok());
        QVERIFY(QFileInfo::exists(target));

        // 只含指定頁，且順序就是使用者給的順序而不是自動排序。
        QCOMPARE(alioth::test::readPageMarkers(target), expectMarkers({5, 1, 3}));
    }

    void extractAcceptsRepeatedPages() {
        // 同一頁擷取兩次是合法需求（做兩份副本），PDFium 的 FPDF_ImportPagesByIndex
        // 不像 FPDF_MovePages 那樣禁止重複索引。
        const QString target = dir_->filePath(QStringLiteral("twice.pdf"));
        const AssemblyResult result =
            extractPages(source_.toStdString(), {1, 1}, target.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.pageCount, 2);
        QCOMPARE(alioth::test::readPageMarkers(target), expectMarkers({2, 2}));
    }

    void extractRejectsOutOfRangeAndEmptySelection() {
        const QString target = dir_->filePath(QStringLiteral("extract.pdf"));
        QCOMPARE(extractPages(source_.toStdString(), {}, target.toStdString()).status,
                 PageEditStatus::InvalidArgument);
        QCOMPARE(extractPages(source_.toStdString(), {99}, target.toStdString()).status,
                 PageEditStatus::InvalidArgument);
        // 驗證未過就不該留下半成品檔案。
        QVERIFY(!QFileInfo::exists(target));
    }

    void extractReportsMissingSource() {
        const QString target = dir_->filePath(QStringLiteral("extract.pdf"));
        const AssemblyResult result = extractPages(
            dir_->filePath(QStringLiteral("nope.pdf")).toStdString(), {0}, target.toStdString());
        QCOMPARE(result.status, PageEditStatus::SourceUnreadable);
    }

    void extractedFileIsIndependentOfSource() {
        const QString target = dir_->filePath(QStringLiteral("extract.pdf"));
        QVERIFY(extractPages(source_.toStdString(), {1}, target.toStdString()).ok());
        QVERIFY(QFile::remove(source_));

        PageEditor editor;
        QVERIFY(editor.open(target.toStdString()));
        QCOMPARE(editor.pageCount(), 1);
    }

    // ---- 合併 -------------------------------------------------------------

    void mergesDocumentsInGivenOrder() {
        const QString first = dir_->filePath(QStringLiteral("a.pdf"));
        const QString second = dir_->filePath(QStringLiteral("b.pdf"));
        const QString target = dir_->filePath(QStringLiteral("merged.pdf"));
        QVERIFY(alioth::test::writePdfTo(first, alioth::test::makeMarkedPdf(2)));
        QVERIFY(alioth::test::writePdfTo(second, alioth::test::makeMarkedPdf(3)));

        const AssemblyResult result =
            mergeDocuments({second.toStdString(), first.toStdString()}, target.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.pageCount, 5);

        // 兩份語料的標記文字相同，因此順序以「3 頁的那份在前」來判定。
        QCOMPARE(alioth::test::readPageMarkers(target), expectMarkers({1, 2, 3, 1, 2}));
    }

    void mergeReportsFormFieldRisk() {
        const QString form = dir_->filePath(QStringLiteral("form.pdf"));
        const QString plain = dir_->filePath(QStringLiteral("plain.pdf"));
        const QString target = dir_->filePath(QStringLiteral("merged.pdf"));
        QVERIFY(alioth::test::writePdfTo(form, alioth::test::makeFormPdf()));
        QVERIFY(alioth::test::writePdfTo(plain, alioth::test::makeMarkedPdf(1)));

        const AssemblyResult result =
            mergeDocuments({form.toStdString(), plain.toStdString()}, target.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.pageCount, 2);
        // Widget 註解會被複製過去，但 PDFium 不合併文件層的 /AcroForm，
        // 因此那個欄位在合併結果裡已經沒有登記。上層必須拿得到這個警訊。
        QCOMPARE(result.forms.widgetAnnotations, 1);
        QVERIFY(result.forms.hasConflictRisk());
        QVERIFY(!result.forms.acroFormMerged);
    }

    void mergeRejectsEmptyAndMissingSources() {
        const QString target = dir_->filePath(QStringLiteral("merged.pdf"));
        QCOMPARE(mergeDocuments({}, target.toStdString()).status, PageEditStatus::InvalidArgument);
        QCOMPARE(mergeDocuments({dir_->filePath(QStringLiteral("nope.pdf")).toStdString()},
                                target.toStdString())
                     .status,
                 PageEditStatus::SourceUnreadable);
    }

    // ---- 分割 -------------------------------------------------------------

    void splitsEveryNPages() {
        const QString pattern = dir_->filePath(QStringLiteral("part-{n}.pdf"));
        const SplitResult result = splitDocument(
            source_.toStdString(), SplitRule{SplitMode::EveryNPages, 4, 0, {}}, pattern.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.outputs.size(), std::size_t{2});
        QCOMPARE(result.outputs[0].pageCount, 4);
        QCOMPARE(result.outputs[1].pageCount, 2);

        QCOMPARE(alioth::test::readPageMarkers(QString::fromStdString(result.outputs[0].path)),
                 expectMarkers({1, 2, 3, 4}));
        QCOMPARE(alioth::test::readPageMarkers(QString::fromStdString(result.outputs[1].path)),
                 expectMarkers({5, 6}));
    }

    void splitsAtGivenPages() {
        const QString pattern = dir_->filePath(QStringLiteral("cut-{n}.pdf"));
        const SplitResult result =
            splitDocument(source_.toStdString(), SplitRule{SplitMode::AtPageNumbers, 1, 0, {1, 5}},
                          pattern.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.outputs.size(), std::size_t{3});
        QCOMPARE(alioth::test::readPageMarkers(QString::fromStdString(result.outputs[0].path)),
                 expectMarkers({1}));
        QCOMPARE(alioth::test::readPageMarkers(QString::fromStdString(result.outputs[1].path)),
                 expectMarkers({2, 3, 4, 5}));
        QCOMPARE(alioth::test::readPageMarkers(QString::fromStdString(result.outputs[2].path)),
                 expectMarkers({6}));
    }

    void splitsByMaxBytes() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        const std::vector<std::uint64_t> pageBytes = editor.estimatePageBytes();
        QCOMPARE(pageBytes.size(), std::size_t{6});
        for (const std::uint64_t bytes : pageBytes) QVERIFY(bytes > 0);
        editor.close();

        // 預算設成「大約兩頁」，切點就必然落在頁與頁之間。
        const std::uint64_t budget = pageBytes[0] + pageBytes[1];
        const QString pattern = dir_->filePath(QStringLiteral("size-{n}.pdf"));
        const SplitResult result =
            splitDocument(source_.toStdString(), SplitRule{SplitMode::MaxBytes, 1, budget, {}},
                          pattern.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());
        QVERIFY(result.outputs.size() >= 2);

        int total = 0;
        for (const SplitOutput& output : result.outputs) {
            QVERIFY(output.pageCount >= 1);
            QVERIFY(QFileInfo::exists(QString::fromStdString(output.path)));
            total += output.pageCount;
        }
        // 分割不得遺漏或憑空多出頁面。
        QCOMPARE(total, 6);
    }

    void splitRejectsInvalidRule() {
        const QString pattern = dir_->filePath(QStringLiteral("bad-{n}.pdf"));
        const SplitResult result = splitDocument(
            source_.toStdString(), SplitRule{SplitMode::EveryNPages, 0, 0, {}}, pattern.toStdString());
        QCOMPARE(result.status, PageEditStatus::InvalidArgument);
        QCOMPARE(result.plan.status, SplitStatus::InvalidChunkSize);
        QVERIFY(result.outputs.empty());
    }

    void splitPathPatternIsPredictable() {
        QCOMPARE(formatSplitPath("out/part-{n}.pdf", 3), std::string("out/part-3.pdf"));
        // 沒有佔位符時序號插在副檔名之前，"a.pdf3" 不是 PDF。
        QCOMPARE(formatSplitPath("out/part.pdf", 2), std::string("out/part-2.pdf"));
        // 目錄名含點、檔名沒有副檔名時不能誤切目錄。
        QCOMPARE(formatSplitPath("a.b/part", 1), std::string("a.b/part-1"));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString source_;
};

QTEST_APPLESS_MAIN(TestPageAssembly)
#include "test_page_assembly.moc"
