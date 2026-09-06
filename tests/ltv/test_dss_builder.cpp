// 長期驗證證據存放區（/DSS）的測試（WBS 6.11，PRD-SIG-006 的 LT 層級）。
//
// 判準：結構面用 qpdf --check 把關；語意面自己用 PdfSourceDocument 重新開檔，
// 確認 /Root /DSS、/Certs /CRLs /OCSPs 陣列、/VRI 字典都指到正確的物件，
// 且共用證據真的只落成一個串流物件（不是每個簽章各自複製一份）。

#include <QtTest>

#include <QDir>
#include <QFile>

#include <cstring>

#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/signature/dss_builder.h"
#include "pdf_fixture.h"
#include "qpdf_check.h"

using namespace alioth::engine::signature;
namespace obj = alioth::engine::objects;

namespace {

[[nodiscard]] std::vector<std::uint8_t> asBytes(const char* text) {
    return std::vector<std::uint8_t>(text, text + std::strlen(text));
}

}  // namespace

class TestDssBuilder : public QObject {
    Q_OBJECT

private slots:
    // SHA1("") 是密碼學界人人皆知的測試向量，用它確認 vriKeyForContents 的
    // 雜湊與十六進位大寫格式都正確，不必自己另外驗證 OpenSSL 的 SHA1。
    void vriKeyMatchesKnownSha1Vector() {
        const std::string key = vriKeyForContents({});
        QCOMPARE(QString::fromStdString(key),
                 QStringLiteral("DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"));
    }

    void appendsDssWithSharedAndPerSignatureEvidence() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        obj::IncrementalAppender appender;
        std::string diagnostic;
        QCOMPARE(appender.open(source, &diagnostic), obj::SourceStatus::Ok);

        const std::vector<std::uint8_t> rootCert = asBytes("fake-root-cert-der");
        const std::vector<std::uint8_t> crl = asBytes("fake-crl-der");
        const std::vector<std::uint8_t> leafCert = asBytes("fake-leaf-cert-der");
        const std::vector<std::uint8_t> ocsp = asBytes("fake-ocsp-response-der");

        DssBuildOptions options;
        options.sharedCerts = {rootCert};
        options.sharedCrls = {crl};

        SignatureEvidence evidence;
        evidence.sha1OfContentsHex = "AABBCCDDEEFF00112233445566778899AABBCCDD";
        // 共用的根憑證再引用一次：appendDocumentSecurityStore 必須認出這是
        // 同一份位元組，只落成一個串流物件，而不是複製第二份。
        evidence.certs = {rootCert, leafCert};
        evidence.ocspResponses = {ocsp};

        options.perSignature = {evidence};

        const DssBuildResult built = appendDocumentSecurityStore(appender, options);
        QVERIFY2(built.ok, built.diagnostic.c_str());
        QCOMPARE(built.certCount, std::size_t{2});  // rootCert + leafCert，去重後
        QCOMPARE(built.crlCount, std::size_t{1});
        QCOMPARE(built.ocspCount, std::size_t{1});
        QVERIFY(built.dssObject > 0);

        const obj::BuildResult result = appender.build();
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.bytes.compare(0, source.size(), source), 0);

        obj::PdfSourceDocument reopened;
        std::string reopenDiagnostic;
        QCOMPARE(reopened.open(result.bytes, &reopenDiagnostic), obj::SourceStatus::Ok);

        const obj::PdfObject* rootEntry = reopened.trailer().find("Root");
        QVERIFY(rootEntry != nullptr);
        const obj::PdfObject catalog = reopened.resolve(*rootEntry);
        const obj::PdfDictionary* catalogDict = catalog.asDictionary();
        QVERIFY(catalogDict != nullptr);
        const obj::PdfObject* dssEntry = catalogDict->find("DSS");
        QVERIFY(dssEntry != nullptr);

        const obj::PdfObject dss = reopened.resolve(*dssEntry);
        const obj::PdfDictionary* dssDict = dss.asDictionary();
        QVERIFY(dssDict != nullptr);

        const obj::PdfObject* certsEntry = dssDict->find("Certs");
        QVERIFY(certsEntry != nullptr);
        QCOMPARE(certsEntry->asArray()->size(), std::size_t{2});

        const obj::PdfObject* crlsEntry = dssDict->find("CRLs");
        QVERIFY(crlsEntry != nullptr);
        QCOMPARE(crlsEntry->asArray()->size(), std::size_t{1});

        const obj::PdfObject* ocspsEntry = dssDict->find("OCSPs");
        QVERIFY(ocspsEntry != nullptr);
        QCOMPARE(ocspsEntry->asArray()->size(), std::size_t{1});

        const obj::PdfObject* vriEntry = dssDict->find("VRI");
        QVERIFY(vriEntry != nullptr);
        const obj::PdfDictionary* vriDict = vriEntry->asDictionary();
        QVERIFY(vriDict != nullptr);
        const obj::PdfObject* signatureVri = vriDict->find(evidence.sha1OfContentsHex);
        QVERIFY(signatureVri != nullptr);
        const obj::PdfDictionary* signatureVriDict = signatureVri->asDictionary();
        QVERIFY(signatureVriDict != nullptr);
        const obj::PdfObject* vriCerts = signatureVriDict->find("Cert");
        QVERIFY(vriCerts != nullptr);
        QCOMPARE(vriCerts->asArray()->size(), std::size_t{2});
        const obj::PdfObject* vriOcsp = signatureVriDict->find("OCSP");
        QVERIFY(vriOcsp != nullptr);
        QCOMPARE(vriOcsp->asArray()->size(), std::size_t{1});
        // 這個簽章沒有自己的 CRL（只有文件層級共用的那一份），/VRI 項不該
        // 出現 /CRL 鍵——寫一個空陣列會讓驗證端誤以為「查過但沒有結果」。
        QVERIFY(signatureVriDict->find("CRL") == nullptr);

        const QString path = QDir::temp().filePath(QStringLiteral(
            "alioth-dss-%1.pdf").arg(QCoreApplication::applicationPid()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(result.bytes));
        }
        const alioth::test::QpdfCheckResult qpdf = alioth::test::runQpdfCheck(path);
        if (qpdf.status == alioth::test::QpdfStatus::NotAvailable) {
            qWarning("%s", alioth::test::qpdfSkipReason().constData());
        } else {
            QVERIFY2(qpdf.clean(), alioth::test::describeQpdfFailure("附加 DSS 的 PDF", qpdf).constData());
        }
        QFile::remove(path);
    }

    void missingVriKeyFailsExplicitly() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        obj::IncrementalAppender appender;
        QCOMPARE(appender.open(source), obj::SourceStatus::Ok);

        DssBuildOptions options;
        SignatureEvidence evidence;  // sha1OfContentsHex 刻意留空
        evidence.certs = {asBytes("x")};
        options.perSignature = {evidence};

        const DssBuildResult built = appendDocumentSecurityStore(appender, options);
        QVERIFY(!built.ok);
        QVERIFY(!built.diagnostic.empty());
    }
};

QTEST_APPLESS_MAIN(TestDssBuilder)
#include "test_dss_builder.moc"
