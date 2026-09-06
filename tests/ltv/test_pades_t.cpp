// PAdES-B-T 端到端測試：建立簽章時一併嵌入 RFC 3161 時間戳
// （WBS 6.11，PRD-SIG-006）。
//
// 判準與 tests/signcreate/test_signature_creator.cpp 一致——不只驗證能不能
// 讀回自己寫的東西，而是走完整鏈路：createSignature() 產生位元組 → 本專案
// 的驗證側（pkcs7_verifier）判斷密碼學有效且偵測到時間戳 → qpdf --check
// 結構乾淨。時間戳來自本地伺服器（timestamp_test_server.h），CI 不連網。

#include <QtTest>

#include <openssl/pem.h>

#include "engine/signature/signature_creator.h"
#include "pdf_fixture.h"
#include "qpdf_check.h"
#include "signature_fixture.h"
#include "timestamp_test_server.h"

using namespace alioth::engine::signature;
using alioth::test::Identity;

namespace {
constexpr long kYear = 365L * 24 * 3600;

[[nodiscard]] std::string privateKeyPem(const Identity& identity) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return {};
    PEM_write_bio_PrivateKey(bio, identity.key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string out;
    if (length > 0 && data) out.assign(data, static_cast<std::size_t>(length));
    BIO_free(bio);
    return out;
}
}  // namespace

class TestPadesT : public QObject {
    Q_OBJECT

private:
    Identity ca_;
    Identity leaf_;
    Identity tsa_;

    void fillSigningIdentity(SigningIdentity& identity) {
        [&] { QVERIFY(identity.loadPem(alioth::test::certificatePem(leaf_), privateKeyPem(leaf_))); }();
        [&] { QVERIFY(identity.addChainCertificatePem(alioth::test::certificatePem(ca_))); }();
    }

private slots:
    void initTestCase() {
        ca_ = alioth::test::makeIdentity("Alioth Test Root CA", nullptr, -kYear, kYear, 1, true);
        QVERIFY(ca_.valid());
        leaf_ = alioth::test::makeIdentity("Mia Legal", &ca_, -3600, kYear, 1002, false);
        QVERIFY(leaf_.valid());
        tsa_ = alioth::test::makeTsaIdentity("Alioth Test TSA", 2002);
        QVERIFY(tsa_.valid());
    }

    void signatureWithTimestampVerifiesAndReportsTimestampPresence() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);
        QVERIFY(identity.valid());

        CreateSignatureOptions options;
        options.reason = "PAdES-B-T test";
        options.signingTimeUnix = 1735732800;
        // CAdES-BES 本身 + 時間戳權杖都要放進同一個 /Contents，保留空間要
        // 比純 SIG-004 的測試（6000）大一截。
        options.reserveBytes = 16000;
        options.timestamp.enabled = true;
        options.timestamp.tsaUrl = "https://tsa.invalid/";
        options.timestamp.transport = [this](const std::string&,
                                             const std::vector<std::uint8_t>& reqDer) {
            return alioth::test::createLocalTimestampResponse(tsa_, reqDer);
        };

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.timestamped);

        const QString path = QDir::temp().filePath(QStringLiteral(
            "alioth-pades-t-%1.pdf").arg(QCoreApplication::applicationPid()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(result.bytes));
        }

        const alioth::test::QpdfCheckResult qpdf = alioth::test::runQpdfCheck(path);
        if (qpdf.status == alioth::test::QpdfStatus::NotAvailable) {
            qWarning("%s", alioth::test::qpdfSkipReason().constData());
        } else {
            QVERIFY2(qpdf.clean(), alioth::test::describeQpdfFailure("PAdES-B-T PDF", qpdf).constData());
        }

        // 驗證側：用專案自己的 signBatch 自我驗證（同一份 pkcs7_verifier，
        // 與 SignatureScanner 走的是同一份驗證程式碼）。
        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions verifyOptions;
        verifyOptions.revocationPolicy = RevocationPolicy::Skip;

        // 直接用 pkcs7_verifier 驗這份位元組（而不是走需要 PDFium 專用執行緒
        // 的 SignatureScanner），手法與 signBatch 內部的自我驗證完全一致。
        const std::vector<int> rawByteRange{
            static_cast<int>(result.byteRange[0]), static_cast<int>(result.byteRange[1]),
            static_cast<int>(result.byteRange[2]), static_cast<int>(result.byteRange[3])};
        const ByteRangeCheck coverage =
            checkByteRange(rawByteRange, static_cast<std::int64_t>(result.bytes.size()));
        QVERIFY2(coverage.ok(), coverage.detail.c_str());

        const auto* data = reinterpret_cast<const std::uint8_t*>(result.bytes.data());
        const std::vector<std::uint8_t> signedBytes =
            assembleSignedBytes(data, result.bytes.size(), coverage);

        const std::int64_t hexBegin = result.byteRange[1] + 1;
        const std::int64_t hexEnd = result.byteRange[2] - 1;
        std::vector<std::uint8_t> contentsDer;
        for (std::int64_t i = hexBegin; i + 1 < hexEnd; i += 2) {
            const auto digit = [&](std::int64_t pos) {
                const char c = result.bytes[static_cast<std::size_t>(pos)];
                return (c >= '0' && c <= '9') ? c - '0' : (c - 'A' + 10);
            };
            contentsDer.push_back(static_cast<std::uint8_t>((digit(i) << 4) | digit(i + 1)));
        }
        const std::vector<std::uint8_t> trimmed = trimDerPadding(contentsDer);

        const SignatureReport report =
            verifyDetachedPkcs7(signedBytes, trimmed, coverage, store, verifyOptions);
        QVERIFY(report.cryptographicallyValid);
        QCOMPARE(report.trust, SignatureTrust::Trusted);
        QVERIFY2(report.hasTimestamp, "驗證側沒有偵測到剛嵌入的時間戳 unsigned attribute");
        // genTime 來自本地 TSA 產生回應當下的系統時間（與 options.signingTimeUnix
        // 無關，那個欄位只影響 /Sig 字典的 /M），只驗證有被填進一個看起來合理的值。
        QVERIFY(report.timestampGenTimeUnix > 0);

        QFile::remove(path);
    }

    void timestampEnabledWithoutTransportFailsExplicitly() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);

        CreateSignatureOptions options;
        options.timestamp.enabled = true;
        options.timestamp.tsaUrl = "https://tsa.invalid/";
        // transport 刻意不設定。

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("傳輸層") != std::string::npos);
        QVERIFY(!result.timestamped);
    }

    void timestampReserveTooSmallFailsExplicitly() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);

        CreateSignatureOptions options;
        options.reserveBytes = 1000;  // 遠不足以放下 CMS + 時間戳權杖
        options.timestamp.enabled = true;
        options.timestamp.tsaUrl = "https://tsa.invalid/";
        options.timestamp.transport = [this](const std::string&,
                                             const std::vector<std::uint8_t>& reqDer) {
            return alioth::test::createLocalTimestampResponse(tsa_, reqDer);
        };

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("保留空間不足") != std::string::npos);
    }
};

QTEST_APPLESS_MAIN(TestPadesT)
#include "test_pades_t.moc"
