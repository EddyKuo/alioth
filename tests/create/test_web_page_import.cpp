// 從網頁 URL 建立 PDF（PRD-IO-014，降級為純文字擷取，WBS 15）。
//
// 這支測試驗的是「降級路徑本身有沒有做對」：下載驗證與 PRD-IO-010 共用，
// 已由 test_url_source.cpp 覆蓋；這裡只驗 HTML 特有的部分——標籤剝除、
// <script>/<style> 排除、實體解碼、以及最終能不能餵進純文字排版產出 PDF。
// 完整 HTML/CSS 渲染的缺口見 domain/document_source.h 與 WP35 報告，
// 不在這支測試的範圍內（因為根本沒有實作）。

#include <QTemporaryDir>
#include <QtTest>

#include <string>

#include "create_test_support.h"
#include "domain/document_source.h"
#include "engine/create/web_page_to_pdf.h"
#include "engine/fonts/cjk_font_library.h"

using namespace alioth::domain::create;
using namespace alioth::engine::create;

namespace {

WebPageFetcher constantFetcher(HttpResponse response) {
    return [response](const std::string&, std::uint64_t) { return response; };
}

HttpResponse htmlResponse(std::string body) {
    HttpResponse response;
    response.transportOk = true;
    response.statusCode = 200;
    response.contentType = "text/html; charset=utf-8";
    response.declaredLength = body.size();
    response.body = std::move(body);
    return response;
}

}  // namespace

class TestWebPageImport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void extractsTitleAndStripsTagsAndScripts() {
        const std::string html =
            "<html><head><title>Report</title><style>body{color:red}</style>"
            "<script>alert('x')</script></head><body>"
            "<h1>Heading</h1><p>Hello &amp; welcome.</p>"
            "<p>Line one<br>Line two</p></body></html>";
        const HtmlExtraction extraction = extractReadableText(html);
        QVERIFY2(extraction.ok, extraction.diagnostic.c_str());
        QVERIFY(extraction.bodyText.find("Heading") != std::string::npos);
        QVERIFY(extraction.bodyText.find("Hello & welcome.") != std::string::npos);
        QVERIFY(extraction.bodyText.find("Line one") != std::string::npos);
        QVERIFY(extraction.bodyText.find("Line two") != std::string::npos);
        QVERIFY2(extraction.bodyText.find("alert") == std::string::npos,
                "<script> 內容不該出現在擷取結果裡");
        QVERIFY2(extraction.bodyText.find("color:red") == std::string::npos,
                "<style> 內容不該出現在擷取結果裡");
    }

    void titleIsExtractedSeparately() {
        const HtmlExtraction extraction =
            extractReadableText("<html><head><title>My Title</title></head><body>Body text</body></html>");
        QVERIFY(extraction.ok);
        QCOMPARE(extraction.title, std::string("My Title"));
        QVERIFY2(extraction.bodyText.find("My Title") == std::string::npos,
                "標題不該重複出現在本文擷取結果裡");
    }

    void nonHttpSchemesAreRejectedBeforeAnyFetch() {
        bool called = false;
        WebPageToPdfConverter converter([&called](const std::string&, std::uint64_t) {
            called = true;
            return HttpResponse{};
        });
        QCOMPARE(converter.precheck("file:///etc/passwd"), HtmlRejection::UnsupportedScheme);
        QVERIFY(!called);
    }

    void wrongContentTypeIsRejected() {
        HttpResponse response = htmlResponse("{}");
        response.contentType = "application/json";
        WebPageToPdfConverter converter(constantFetcher(response));
        const WebPageImportResult result = converter.convert("https://example.invalid/a.json");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, HtmlRejection::ContentTypeMismatch);
    }

    void emptyExtractedTextIsRejected() {
        WebPageToPdfConverter converter(
            constantFetcher(htmlResponse("<html><head><script>1</script></head><body></body></html>")));
        const WebPageImportResult result = converter.convert("https://example.invalid/empty.html");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, HtmlRejection::NoExtractableText);
    }

    void convertProducesAValidPdf() {
        const std::string html =
            "<html><head><title>Alioth Test Page</title></head>"
            "<body><h1>Section</h1><p>This is a paragraph with plain ASCII text.</p></body></html>";
        WebPageToPdfConverter converter(constantFetcher(htmlResponse(html)));
        const WebPageImportResult result = converter.convert("https://example.invalid/page.html");
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.pageTitle, std::string("Alioth Test Page"));
        QVERIFY(result.pageCount >= 1);

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("webpage.pdf"), result.bytes);
        QVERIFY(!path.isEmpty());
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("網頁匯入"));
    }

    void nonAsciiHtmlContentEmbedsCjkFont() {
        // 中文內容通過 HTML 擷取後，交給純文字排版走內嵌子集（ADR-007）。
        // 這不是這支功能自己的規則，而是跟著 createPdfFromPlainText 走——
        // 網頁匯入不該有一套自己的字型政策。
        const std::string html = "<html><body><p>\xE4\xB8\xAD\xE6\x96\x87\xE5\x85\xA7\xE5\xAE\xB9</p></body></html>";
        WebPageToPdfConverter converter(constantFetcher(htmlResponse(html)));
        const WebPageImportResult result = converter.convert("https://example.invalid/cjk.html");

        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            // 字型不在時必須明確失敗，不可退回拉丁字型畫出一排空框。
            QVERIFY(!result.ok);
            return;
        }

        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.bytes.find("/CJK") != std::string::npos);

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("webpage_cjk.pdf"), result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("含 CJK 的網頁匯入"));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestWebPageImport)
#include "test_web_page_import.moc"
