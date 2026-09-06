// 從 URL 開啟文件的測試（PRD-IO-010，WBS 15）。
//
// 全部走注入的假傳輸，一個位元組都不連網。這不是為了跑得快——是因為
// 連網的測試驗的是「今天網路通不通」，而我們要驗的是策略：哪些結構描述
// 放行、多大算太大、內容型別對不上怎麼辦。那些規則在真實網路上反而
// 難以重現。

#include <QTemporaryDir>
#include <QtTest>

#include <string>

#include "create_test_support.h"
#include "domain/document_source.h"
#include "engine/create/text_to_pdf.h"
#include "engine/create/url_document_source.h"

using namespace alioth::domain::create;
using namespace alioth::engine::create;

namespace {

std::string samplePdf() {
    const TextImportResult result = createPdfFromPlainText("Fetched over HTTP.");
    return result.ok ? result.bytes : std::string{};
}

HttpFetcher constantFetcher(HttpResponse response) {
    return [response](const std::string&, std::uint64_t) { return response; };
}

HttpResponse okResponse(std::string body) {
    HttpResponse response;
    response.transportOk = true;
    response.statusCode = 200;
    response.contentType = "application/pdf";
    response.declaredLength = body.size();
    response.body = std::move(body);
    return response;
}

}  // namespace

class TestUrlSource : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        pdf_ = samplePdf();
        QVERIFY(!pdf_.empty());
    }

    // file:// 與 data: 是本地檔案與內嵌內容，不該經過「從網址開啟」這條路；
    // 放行它們等於把任意本地讀取包裝成一個看起來無害的網址欄位。
    void nonHttpSchemesAreRejected() {
        UrlDocumentOpener opener(constantFetcher(okResponse(pdf_)));
        opener.setTemporaryDirectory(dir_->path().toStdString());

        for (const char* url : {"file:///C:/secret.pdf", "ftp://host/a.pdf",
                                "data:application/pdf;base64,AAAA", "javascript://x"}) {
            const UrlOpenResult result = opener.open(url);
            QVERIFY2(!result.ok, url);
            QVERIFY2(result.rejection == UrlRejection::UnsupportedScheme ||
                         result.rejection == UrlRejection::MalformedUrl,
                     url);
        }
    }

    void emptyAndMalformedUrlsAreRejected() {
        UrlDocumentOpener opener(constantFetcher(okResponse(pdf_)));
        QCOMPARE(opener.open("").rejection, UrlRejection::EmptyUrl);
        QCOMPARE(opener.open("not a url").rejection, UrlRejection::MalformedUrl);
        QCOMPARE(opener.open("https:///only-a-path").rejection, UrlRejection::HostMissing);
    }

    // 明文 http 可以整批關掉（企業部署常見的要求）。
    void plainHttpCanBeDisallowed() {
        UrlFetchPolicy policy;
        policy.allowPlainHttp = false;
        UrlDocumentOpener opener(constantFetcher(okResponse(pdf_)), policy);
        QCOMPARE(opener.open("http://host/a.pdf").rejection, UrlRejection::HttpNotAllowed);
        opener.setTemporaryDirectory(dir_->path().toStdString());
        QVERIFY(opener.open("https://host/a.pdf").ok);
    }

    // 伺服器宣告的長度就超過上限時，連下載都不必開始。
    void declaredSizeOverLimitIsRejected() {
        UrlFetchPolicy policy;
        policy.maxBytes = 1024;
        HttpResponse response = okResponse(pdf_);
        response.declaredLength = 5ull * 1024 * 1024;

        UrlDocumentOpener opener(constantFetcher(response), policy);
        opener.setTemporaryDirectory(dir_->path().toStdString());
        const UrlOpenResult result = opener.open("https://host/big.pdf");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, UrlRejection::DeclaredSizeTooLarge);
        QVERIFY(result.temporaryPath.empty());
    }

    // 伺服器少報或不報長度時，實際位元組數仍然要擋。上限只寫在
    // Content-Length 上等於相信對方的自我申報。
    void actualBodyOverLimitIsRejected() {
        UrlFetchPolicy policy;
        policy.maxBytes = 64;
        HttpResponse response = okResponse(pdf_);
        response.declaredLength = 0;  // 伺服器沒給

        UrlDocumentOpener opener(constantFetcher(response), policy);
        opener.setTemporaryDirectory(dir_->path().toStdString());
        const UrlOpenResult result = opener.open("https://host/big.pdf");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, UrlRejection::BodyTooLarge);
    }

    void contentTypeMismatchIsRejected() {
        HttpResponse response = okResponse(pdf_);
        response.contentType = "text/html; charset=utf-8";

        UrlDocumentOpener opener(constantFetcher(response));
        opener.setTemporaryDirectory(dir_->path().toStdString());
        const UrlOpenResult result = opener.open("https://host/page.html");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, UrlRejection::ContentTypeMismatch);
        QVERIFY(result.diagnostic.find("text/html") != std::string::npos);
    }

    // 帶參數的 Content-Type 與 octet-stream 都要放行，真正的把關是簽章檢查。
    void acceptableContentTypesPass() {
        for (const char* type : {"application/pdf", "application/pdf; charset=binary",
                                 "APPLICATION/PDF", "application/octet-stream"}) {
            HttpResponse response = okResponse(pdf_);
            response.contentType = type;
            UrlDocumentOpener opener(constantFetcher(response));
            opener.setTemporaryDirectory(dir_->path().toStdString());
            const UrlOpenResult result = opener.open("https://host/a.pdf");
            QVERIFY2(result.ok, type);
            UrlDocumentOpener::discard(result);
        }
    }

    // Content-Type 說是 PDF，內容卻不是。這是最需要擋的一種：
    // 型別標頭是對方說了算，檔案開頭不是。
    void lyingContentTypeIsCaughtBySignature() {
        HttpResponse response = okResponse("<html><body>not a pdf</body></html>");
        UrlDocumentOpener opener(constantFetcher(response));
        opener.setTemporaryDirectory(dir_->path().toStdString());
        const UrlOpenResult result = opener.open("https://host/a.pdf");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, UrlRejection::NotPdfSignature);
    }

    void emptyBodyIsRejected() {
        UrlDocumentOpener opener(constantFetcher(okResponse("")));
        opener.setTemporaryDirectory(dir_->path().toStdString());
        QCOMPARE(opener.open("https://host/a.pdf").rejection, UrlRejection::EmptyBody);
    }

    void httpErrorStatusIsRejected() {
        HttpResponse response = okResponse(pdf_);
        response.statusCode = 404;
        UrlDocumentOpener opener(constantFetcher(response));
        const UrlOpenResult result = opener.open("https://host/missing.pdf");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, UrlRejection::HttpStatusNotOk);
        QVERIFY(result.diagnostic.find("404") != std::string::npos);
    }

    void transportFailureIsRejected() {
        HttpResponse response;
        response.transportOk = false;
        UrlDocumentOpener opener(constantFetcher(response));
        QCOMPARE(opener.open("https://host/a.pdf").rejection, UrlRejection::TransportFailed);
    }

    // 沒注入傳輸不能靜默成功。把「沒下載」講成「沒問題」正是三態要防的事。
    void missingTransportFailsLoudly() {
        UrlDocumentOpener opener;
        const UrlOpenResult result = opener.open("https://host/a.pdf");
        QVERIFY(!result.ok);
        QCOMPARE(result.rejection, UrlRejection::TransportFailed);
        QVERIFY(result.diagnostic.find("未注入") != std::string::npos);
    }

    // precheck 不得連網：UI 要在問使用者之前就擋掉明顯不合法的網址。
    void precheckDoesNotTouchTransport() {
        bool called = false;
        UrlDocumentOpener opener([&called](const std::string&, std::uint64_t) {
            called = true;
            return HttpResponse{};
        });
        QCOMPARE(opener.precheck("file:///etc/passwd"), UrlRejection::UnsupportedScheme);
        QCOMPARE(opener.precheck("https://host/a.pdf"), UrlRejection::None);
        QVERIFY2(!called, "precheck 不應該發出任何請求");
    }

    // 傳輸端收到的上限提示要與策略一致，否則它會先收完整份再讓上層丟掉。
    void fetcherReceivesTheSizeLimit() {
        std::uint64_t seen = 0;
        UrlFetchPolicy policy;
        policy.maxBytes = 4096;
        UrlDocumentOpener opener(
            [&seen, this](const std::string&, std::uint64_t maxBytes) {
                seen = maxBytes;
                return okResponse(pdf_);
            },
            policy);
        opener.setTemporaryDirectory(dir_->path().toStdString());
        QVERIFY(opener.open("https://host/a.pdf").ok);
        QCOMPARE(seen, std::uint64_t{4096});
    }

    // 全程走完：下載到暫存檔、位元組與來源相同、PDFium 開得起來、
    // qpdf 零警告，而且 discard 之後檔案真的消失。
    void successfulFetchLandsInATemporaryFile() {
        UrlDocumentOpener opener(constantFetcher(okResponse(pdf_)));
        opener.setTemporaryDirectory(dir_->path().toStdString());

        const UrlOpenResult result = opener.open("https://example.invalid/doc.pdf");
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.byteCount, static_cast<std::uint64_t>(pdf_.size()));

        const QString path = QString::fromStdString(result.temporaryPath.string());
        QVERIFY(QFileInfo::exists(path));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray written = file.readAll();
        file.close();
        // 二進位落檔：任何一個 0x0A 被展開成 0x0D 0x0A，xref 位移就全錯了。
        QCOMPARE(written.size(), static_cast<qsizetype>(pdf_.size()));
        QCOMPARE(written, QByteArray(pdf_.data(), static_cast<qsizetype>(pdf_.size())));

        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("從 URL 下載的文件"));

        UrlDocumentOpener::discard(result);
        QVERIFY(!QFileInfo::exists(path));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    std::string pdf_;
};

QTEST_MAIN(TestUrlSource)
#include "test_url_source.moc"
