// 建立數位簽章的端到端測試（WBS 6.11，PRD-SIG-004 / PRD-SIG-007）。
//
// 判準比一般測試嚴格：只驗證「我們自己的程式碼可以讀回自己簽的東西」沒有
// 意義，因此這裡走的是完整鏈路——
//   1. 用 createSignature() 產生簽過的 PDF（物件層，不碰 PDFium）
//   2. 用 SignatureScanner（PDFium 列舉 + OpenSSL 驗證，與 tests/signature 相同
//      的驗證側）重新驗證，證明密碼學上真的有效、鏈可信、涵蓋完整
//   3. 用 qpdf --check（獨立於 PDFium 的第二意見）確認檔案結構本身合法
//
// 憑證語料沿用 tests/signature/signature_fixture.h 的做法：自己造一組自簽 CA
// 與簽署憑證，不 commit 任何私鑰檔案——整個測試執行期間金鑰只存在於記憶體。

#include <QtTest>

#include <openssl/bio.h>
#include <openssl/pem.h>

#include <condition_variable>
#include <mutex>

#include "engine/objects/pdf_source_document.h"
#include "engine/signature/signature_creator.h"
#include "engine/signature/signature_scanner.h"
#include "engine/signature/signing_identity.h"
#include "pdf_fixture.h"
#include "qpdf_check.h"
#include "signature_fixture.h"

using namespace alioth::engine::signature;
using alioth::domain::DocumentError;
using alioth::test::Identity;

namespace {

constexpr long kYear = 365L * 24 * 3600;

template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }
    [[nodiscard]] bool wait(int milliseconds = 20000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }
    [[nodiscard]] const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

// signature_fixture.h 只公開憑證的 PEM（certificatePem），沒有私鑰的 PEM——
// 驗證側從來不需要它。建立簽章的測試需要，因此在這裡自己補一個，
// 不改動既有的驗證側語料產生器。
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

class TestSignatureCreator : public QObject {
    Q_OBJECT

private:
    Identity ca_;
    Identity leaf_;

    static bool openScanner(SignatureScanner& scanner, const QString& path) {
        Latch<DocumentError> latch;
        scanner.open(path.toStdString(), {}, [&latch](DocumentError error) { latch.set(error); });
        if (!latch.wait()) return false;
        return latch.value() == DocumentError::None;
    }

    static std::vector<SignatureReport> scanWith(SignatureScanner& scanner,
                                                 const TrustStore& store, VerifyOptions options) {
        Latch<std::vector<SignatureReport>> latch;
        scanner.scan(&store, options, nullptr,
                     [&latch](std::vector<SignatureReport> reports) {
                         latch.set(std::move(reports));
                     });
        if (!latch.wait()) return {};
        return latch.value();
    }

    // SigningIdentity 刻意不可複製也不可搬移（私鑰只活在單一實例裡），
    // 因此用輸出參數而不是回傳值——回傳值會需要一個可用的搬移建構子。
    void fillSigningIdentity(SigningIdentity& identity) {
        [&] {
            QVERIFY(identity.loadPem(alioth::test::certificatePem(leaf_), privateKeyPem(leaf_)));
        }();
        [&] { QVERIFY(identity.addChainCertificatePem(alioth::test::certificatePem(ca_))); }();
    }

private slots:
    void initTestCase() {
        ca_ = alioth::test::makeIdentity("Alioth Test Root CA", nullptr, -kYear, kYear, 1, true);
        QVERIFY(ca_.valid());
        leaf_ = alioth::test::makeIdentity("Mia Legal", &ca_, -3600, kYear, 1001, false);
        QVERIFY(leaf_.valid());
    }

    // PRD-SIG-004：一般（非認證）簽章要能被我們自己的驗證側判成綠燈，
    // 且 qpdf --check 找不到結構問題。
    void plainSignatureVerifiesEndToEnd() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);
        QVERIFY(identity.valid());

        CreateSignatureOptions options;
        options.reason = "Alioth test";
        options.location = "Taipei";
        options.signingTimeUnix = 1735732800;  // 固定時間，斷言才能重現

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.appendedBytes > 0);
        // PRD-IO-001 的精神延伸到這裡：新增一個簽章不該讓檔案暴增。
        QVERIFY(result.appendedBytes < 20000);

        const QString path = QDir::temp().filePath(
            QStringLiteral("alioth-signcreate-plain-%1.pdf").arg(QCoreApplication::applicationPid()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(result.bytes));
        }

        const alioth::test::QpdfCheckResult qpdf = alioth::test::runQpdfCheck(path);
        if (qpdf.status == alioth::test::QpdfStatus::NotAvailable) {
            qWarning("%s", alioth::test::qpdfSkipReason().constData());
        } else {
            QVERIFY2(qpdf.clean(), alioth::test::describeQpdfFailure("建立的簽章 PDF", qpdf).constData());
        }

        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, path));
        QCOMPARE(scanner.signatureCount(), 1);

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions verifyOptions;
        verifyOptions.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, store, verifyOptions);
        QCOMPARE(reports.size(), std::size_t{1});

        const SignatureReport& report = reports.front();
        QCOMPARE(report.subFilter, std::string("ETSI.CAdES.detached"));
        QCOMPARE(report.reason, std::string("Alioth test"));
        QVERIFY2(report.coverage.ok(), report.coverage.detail.c_str());
        QVERIFY(report.parsed);
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.chainTrusted);
        QCOMPARE(report.signerName, std::string("Mia Legal"));
        QCOMPARE(report.trust, SignatureTrust::Trusted);

        QFile::remove(path);
    }

    // PRD-SIG-004 的補完：可見簽章外觀（見 ADR-004）。外觀是給人看的，
    // 沒有安全意義；因此這裡驗的是「加了外觀之後密碼學保證完全不變」——
    // 外觀寫壞而讓簽章失效，是這條路最可能造成的傷害。
    void visibleSignatureKeepsCryptographyIntact() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);
        QVERIFY(identity.valid());

        CreateSignatureOptions options;
        options.reason = "Design review";
        options.signingTimeUnix = 1735732800;
        options.appearance.rectPt = alioth::domain::RectF{72.0, 72.0, 312.0, 132.0};
        options.appearance.signerName = "Mia Legal";
        options.appearance.signingTime = "2025-01-01 12:00:00 +00:00";
        options.appearance.reason = "Design review";

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        // /Rect 不再是全零，而且 /AP 真的掛上去了——少了任何一個，
        // 簽章欄在畫面上都是看不見的，而且沒有錯誤訊息。
        QVERIFY(result.bytes.find("/AP") != std::string::npos);
        QVERIFY(result.bytes.find("/Helv") != std::string::npos);
        QVERIFY(result.bytes.find("(Mia Legal) Tj") != std::string::npos);

        const QString path = QDir::temp().filePath(
            QStringLiteral("alioth-signcreate-visible-%1.pdf")
                .arg(QCoreApplication::applicationPid()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(result.bytes));
        }

        const alioth::test::QpdfCheckResult qpdf = alioth::test::runQpdfCheck(path);
        if (qpdf.status == alioth::test::QpdfStatus::NotAvailable) {
            qWarning("%s", alioth::test::qpdfSkipReason().constData());
        } else {
            QVERIFY2(qpdf.clean(),
                     alioth::test::describeQpdfFailure("可見簽章 PDF", qpdf).constData());
        }

        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, path));
        QCOMPARE(scanner.signatureCount(), 1);

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions verifyOptions;
        verifyOptions.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, store, verifyOptions);
        QCOMPARE(reports.size(), std::size_t{1});
        const SignatureReport& report = reports.front();
        QVERIFY2(report.coverage.ok(), report.coverage.detail.c_str());
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QCOMPARE(report.trust, SignatureTrust::Trusted);

        QFile::remove(path);
    }

    // PRD-SIG-007：認證簽章要能在目錄寫出 /Perms /DocMDP，且密碼學驗證仍然通過
    // （DocMDP 本身是否被 Acrobat 正確解讀為「認證」超出本測試能力範圍，
    //  見交付報告 BLOCKED 清單）。
    void certifySignatureWritesDocMdpAndVerifies() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);

        CreateSignatureOptions options;
        options.reason = "Certify test";
        options.certify = CertifyLevel::FormFillingAndAnnotations;
        options.documentAlreadyHasSignatures = false;
        options.signingTimeUnix = 1735732800;

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        // 結構面：/Root /Perms /DocMDP 必須指到剛建立的簽章物件。
        alioth::engine::objects::PdfSourceDocument reopened;
        std::string diagnostic;
        QCOMPARE(reopened.open(result.bytes, &diagnostic), alioth::engine::objects::SourceStatus::Ok);
        const alioth::engine::objects::PdfObject* rootEntry = reopened.trailer().find("Root");
        QVERIFY(rootEntry != nullptr);
        const alioth::engine::objects::PdfObject root = reopened.resolve(*rootEntry);
        const alioth::engine::objects::PdfDictionary* catalog = root.asDictionary();
        QVERIFY(catalog != nullptr);
        const alioth::engine::objects::PdfObject* permsEntry = catalog->find("Perms");
        QVERIFY(permsEntry != nullptr);
        const alioth::engine::objects::PdfObject perms = reopened.resolve(*permsEntry);
        const alioth::engine::objects::PdfDictionary* permsDict = perms.asDictionary();
        QVERIFY(permsDict != nullptr);
        const alioth::engine::objects::PdfObject* docMdp = permsDict->find("DocMDP");
        QVERIFY(docMdp != nullptr);
        QCOMPARE(docMdp->asRef().number, result.signatureDictObject);

        // 密碼學面：即使多了 /Reference /DocMDP，簽章依然要能通過我們自己的驗證側。
        const QString path = QDir::temp().filePath(QStringLiteral(
            "alioth-signcreate-certify-%1.pdf").arg(QCoreApplication::applicationPid()));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(result.bytes));
        }
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, path));
        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions verifyOptions;
        verifyOptions.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, store, verifyOptions);
        QCOMPARE(reports.size(), std::size_t{1});
        QVERIFY(reports.front().cryptographicallyValid);
        QCOMPARE(reports.front().trust, SignatureTrust::Trusted);

        QFile::remove(path);
    }

    // 認證簽章必須是文件的第一個簽章；呼叫端已明講文件已有簽章時必須直接拒絕，
    // 不得矇混寫出一份看似認證、實則違反規格的檔案。
    void certifyRefusesWhenDocumentAlreadySigned() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);

        CreateSignatureOptions options;
        options.certify = CertifyLevel::NoChangesAllowed;
        options.documentAlreadyHasSignatures = true;

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    // /Contents 保留空間必須明確回報不足，而不是靜默截斷簽章值——
    // 截斷的 DER 會讓 d2i_PKCS7 直接解析失敗，症狀會被誤診成別的原因。
    void tooSmallReserveFailsExplicitly() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;
        fillSigningIdentity(identity);

        CreateSignatureOptions options;
        options.reserveBytes = 256;  // 兩張憑證的 CAdES-BES 簽章遠超過這個大小

        const CreateSignatureResult result = createSignature(source, identity, options);
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("保留空間不足") != std::string::npos);
    }

    void refusesWithoutValidIdentity() {
        const std::string source = alioth::test::makeSinglePagePdf().toStdString();
        SigningIdentity identity;  // 未載入任何憑證/私鑰
        const CreateSignatureResult result = createSignature(source, identity, CreateSignatureOptions{});
        QVERIFY(!result.ok);
    }
};

QTEST_APPLESS_MAIN(TestSignatureCreator)
#include "test_signature_creator.moc"
