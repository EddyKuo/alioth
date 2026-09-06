// 新增條碼（PRD-ENH-007，WP35）。
//
// 驗到「頁面內容真的多了一段畫條碼的內容串流」與「矩形太小時明確失敗」，
// 條碼編碼本身的正確性由 tests/test_barcode.cpp 覆蓋，這裡不重複。

#include <QtTest>

#include "domain/barcode.h"
#include "engine/enhance/barcode_stamp.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"

using namespace alioth;
using namespace alioth::engine::enhance;

namespace {

std::string minimalOnePagePdf() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
                      "/Resources << >> >>");
    const std::string content = "% empty page\n";
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

class TestBarcodeStamp : public QObject {
    Q_OBJECT

private slots:
    void stampsBarcodeContentOntoPage() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(minimalOnePagePdf()), engine::objects::SourceStatus::Ok);

        const BarcodeStampResult result =
            stampBarcode(appender, 0, "ORDER-99", alioth::domain::RectF{20.0, 20.0, 220.0, 80.0});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.contentObject > 0);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        const engine::objects::PdfObject contentObject = output.object(result.contentObject);
        const engine::objects::PdfStream* stream = contentObject.asStream();
        QVERIFY(stream != nullptr);
        // 人讀文字必須出現在內容串流裡（沒有被跳過），否則使用者看不到
        // 掃描器讀不出來時可以核對的文字。
        QVERIFY2(stream->data.find("ORDER-99") != std::string::npos, stream->data.c_str());
        QVERIFY2(stream->data.find(" re\n") != std::string::npos, "沒有畫出任何條碼矩形");
    }

    void tooSmallRectFailsExplicitly() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(minimalOnePagePdf()), engine::objects::SourceStatus::Ok);

        // 高度只有 5pt，扣掉人讀文字列之後不夠放條碼。
        const BarcodeStampResult result =
            stampBarcode(appender, 0, "X", alioth::domain::RectF{0.0, 0.0, 100.0, 5.0});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void nonAsciiTextFailsExplicitly() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(minimalOnePagePdf()), engine::objects::SourceStatus::Ok);
        const BarcodeStampResult result =
            stampBarcode(appender, 0, "\xE4\xB8\xAD\xE6\x96\x87", alioth::domain::RectF{0, 0, 200, 80});
        QVERIFY(!result.ok);
    }

    void withoutHumanReadableTextOmitsTextOperators() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(minimalOnePagePdf()), engine::objects::SourceStatus::Ok);

        BarcodeStampOptions options;
        options.includeHumanReadableText = false;
        const BarcodeStampResult result =
            stampBarcode(appender, 0, "NOTEXT", alioth::domain::RectF{0.0, 0.0, 200.0, 60.0}, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);
        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        const engine::objects::PdfStream* stream =
            output.object(result.contentObject).asStream();
        QVERIFY(stream != nullptr);
        QVERIFY(stream->data.find("BT") == std::string::npos);
        QVERIFY(stream->data.find("NOTEXT") == std::string::npos);
    }
};

QTEST_MAIN(TestBarcodeStamp)
#include "test_barcode_stamp.moc"
