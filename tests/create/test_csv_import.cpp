// 從 CSV 建立 PDF（PRD-IO-013 的 CSV 部分，WBS 15）。

#include <QTemporaryDir>
#include <QtTest>

#include <string>

#include "create_test_support.h"
#include "domain/document_source.h"
#include "engine/create/csv_to_pdf.h"

using namespace alioth::domain::create;
using namespace alioth::engine::create;

class TestCsvImport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void basicTableProducesOnePage() {
        const std::string csv = "Name,Amount,Note\r\nAlice,100,ok\r\nBob,200,late\r\n";
        const CsvImportResult result = createPdfFromCsv(csv);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.columnCount, std::size_t{3});
        QCOMPARE(result.rowCount, std::size_t{2});
        QCOMPARE(result.pageCount, std::size_t{1});

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("basic.pdf"), result.bytes);
        QVERIFY(!path.isEmpty());
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("CSV 匯入"));
    }

    void headerRepeatsOnEveryPage() {
        // 版心夠小、列數夠多，逼出至少兩頁，驗證表頭有沒有跟著重複。
        CsvImportOptions options;
        options.paper = PaperSize{200.0, 160.0};
        options.marginPt = 10.0;
        options.fontSize = 8.0;

        std::string csv = "H1,H2\r\n";
        for (int i = 0; i < 30; ++i) {
            csv += "row" + std::to_string(i) + ",v" + std::to_string(i) + "\r\n";
        }

        const CsvTableLayout layout = layoutCsvTable(csv, options);
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QVERIFY2(layout.pageCount() >= 2, "測資沒有逼出多頁，測試本身需要調整版心");
        QCOMPARE(layout.headerRow.size(), std::size_t{2});
        QCOMPARE(layout.headerRow[0], std::string("H1"));

        // 每一頁都應該有本文列（表頭由 layout.headerRow 統一提供，不重複存在
        // pages[i].rows 裡）。
        for (const CsvPage& page : layout.pages) QVERIFY(!page.rows.empty());
    }

    void overlyLongCellIsTruncatedWithEllipsis() {
        CsvImportOptions options;
        options.maxColumnWidthPt = 60.0;
        options.minColumnWidthPt = 60.0;  // 固定欄寬，方便斷言一定會截斷
        const std::string csv =
            "Col\r\nThis is a very long cell value that will not fit in sixty points\r\n";
        const CsvTableLayout layout = layoutCsvTable(csv, options);
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QCOMPARE(layout.pages.size(), std::size_t{1});
        QCOMPARE(layout.pages[0].rows.size(), std::size_t{1});
        const std::string& cell = layout.pages[0].rows[0][0];
        QVERIFY2(cell.size() < std::string("This is a very long cell value that will not fit in sixty points").size(),
                 cell.c_str());
        QVERIFY2(cell.rfind("...") == cell.size() - 3, cell.c_str());
    }

    void columnWidthsShrinkProportionallyToFitPage() {
        CsvImportOptions options;
        options.paper = kPaperA4;
        options.marginPt = 36.0;
        // 十個很寬的欄位：自然寬度總和一定超過版心。
        std::string header;
        std::string row;
        for (int i = 0; i < 10; ++i) {
            if (i > 0) { header += ","; row += ","; }
            header += "Column Header " + std::to_string(i);
            row += "Some fairly long value " + std::to_string(i);
        }
        const CsvTableLayout layout = layoutCsvTable(header + "\r\n" + row + "\r\n", options);
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        double total = 0.0;
        for (const double w : layout.columnWidthsPt) total += w;
        QVERIFY2(total <= kPaperA4.widthPt - options.marginPt * 2.0 + 0.5, "欄寬總和超出版心");
    }

    void nonAsciiContentFailsExplicitly() {
        const CsvImportResult result = createPdfFromCsv("Name\r\n\xE4\xB8\xAD\xE6\x96\x87\r\n");
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void emptyCsvFailsExplicitly() {
        const CsvImportResult result = createPdfFromCsv("");
        QVERIFY(!result.ok);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestCsvImport)
#include "test_csv_import.moc"
