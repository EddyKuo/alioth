// 簽章列舉與端到端驗證的測試（WBS 6.5–6.7，PRD-SIG-001 / 002）。
//
// 這一組會產生一份**真的簽過的 PDF**：自簽 CA 簽發簽署憑證、依 /ByteRange
// 對實際檔案位元組做 detached PKCS#7、把 DER 填回 /Contents。
// 有了可控的正例，「改一個位元組必須失敗」才是有意義的斷言。

#include <QtTest>

#include <condition_variable>
#include <mutex>

#include "engine/signature/signature_scanner.h"
#include "pdf_fixture.h"
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

}  // namespace

class TestSignatureScanner : public QObject {
    Q_OBJECT

private:
    Identity ca_;
    Identity leaf_;
    Identity expired_;

    static bool openScanner(SignatureScanner& scanner, QTemporaryFile& file) {
        Latch<DocumentError> latch;
        scanner.open(file.fileName().toStdString(), {},
                     [&latch](DocumentError error) { latch.set(error); });
        if (!latch.wait()) return false;
        return latch.value() == DocumentError::None;
    }

    static std::vector<SignatureReport> scanWith(SignatureScanner& scanner,
                                                 const TrustStore& store, VerifyOptions options,
                                                 RevocationChecker* revocation = nullptr) {
        Latch<std::vector<SignatureReport>> latch;
        scanner.scan(&store, options, revocation,
                     [&latch](std::vector<SignatureReport> reports) {
                         latch.set(std::move(reports));
                     });
        if (!latch.wait()) return {};
        return latch.value();
    }

    [[nodiscard]] TrustStore* trustingCa() {
        auto* store = new TrustStore();
        [&] { QVERIFY(store->addCertificatePem(alioth::test::certificatePem(ca_))); }();
        return store;
    }

private slots:
    void initTestCase() {
        ca_ = alioth::test::makeIdentity("Alioth Test Root CA", nullptr, -kYear, kYear, 1, true);
        QVERIFY(ca_.valid());
        leaf_ = alioth::test::makeIdentity("Mia Legal", &ca_, -3600, kYear, 1001, false);
        QVERIFY(leaf_.valid());
        expired_ =
            alioth::test::makeIdentity("Expired Signer", &ca_, -2 * kYear, -kYear, 3001, false);
        QVERIFY(expired_.valid());
    }

    void documentWithoutSignaturesReportsZero() {
        auto file = alioth::test::writeTempPdf(alioth::test::makeSinglePagePdf());
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));
        QCOMPARE(scanner.signatureCount(), 0);

        TrustStore store;
        QVERIFY(scanWith(scanner, store, VerifyOptions{}).empty());
    }

    void signedDocumentVerifiesEndToEnd() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY2(signed_.ok, "簽章語料產生失敗，後面的斷言都沒有意義");

        auto file = alioth::test::writeTempPdf(signed_.bytes);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));
        QCOMPARE(scanner.signatureCount(), 1);

        std::unique_ptr<TrustStore> store(trustingCa());
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, *store, options);
        QCOMPARE(reports.size(), std::size_t{1});

        const SignatureReport& report = reports.front();
        QCOMPARE(report.subFilter, std::string("adbe.pkcs7.detached"));
        QCOMPARE(report.signingTimeRaw, std::string("D:20260101120000+08'00'"));
        QCOMPARE(report.reason, std::string("Alioth test"));
        QVERIFY2(report.coverage.ok(), report.coverage.detail.c_str());
        QVERIFY(report.parsed);
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.chainTrusted);
        QCOMPARE(report.signerName, std::string("Mia Legal"));
        QCOMPARE(report.trust, SignatureTrust::Trusted);
    }

    void singleFlippedByteBreaksTheSignature() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY(signed_.ok);

        QByteArray tampered = signed_.bytes;
        // 改頁面內容串流裡的一個位元組。它在 /ByteRange 的第一段內，
        // 所以摘要必定改變——這是竄改偵測的最小案例。
        const int contentPos = tampered.indexOf("20 20 160 100 re");
        QVERIFY(contentPos > 0);
        tampered[contentPos] = '3';

        auto file = alioth::test::writeTempPdf(tampered);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));

        std::unique_ptr<TrustStore> store(trustingCa());
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, *store, options);
        QCOMPARE(reports.size(), std::size_t{1});
        QVERIFY(reports.front().parsed);
        QVERIFY(!reports.front().digestMatches);
        QCOMPARE(reports.front().trust, SignatureTrust::Invalid);
    }

    void appendedContentIsReportedAsPartialCoverage() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY(signed_.ok);

        const QByteArray attacked = alioth::test::appendUncoveredContent(signed_.bytes);
        QVERIFY(attacked.size() > signed_.bytes.size());

        auto file = alioth::test::writeTempPdf(attacked);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));
        QCOMPARE(scanner.signatureCount(), 1);

        std::unique_ptr<TrustStore> store(trustingCa());
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, *store, options);
        QCOMPARE(reports.size(), std::size_t{1});

        const SignatureReport& report = reports.front();
        // 密碼學驗證仍然完全通過——附加的位元組不在簽章涵蓋範圍內。
        // 這正是為什麼「簽章有效」不能只看密碼學結果。
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY2(report.coverage.partiallyCovered(),
                 "尾端附加的內容未被偵測為未涵蓋，這是簽章偽造的直接入口");
        QVERIFY(report.coverage.uncoveredBytes() > 0);
        // 黃燈而不是紅燈。紅燈的語意是「這份簽章無效」，而它並不無效——
        // 摘要對得上。真正的情況是我們無法確認那段附加內容是無害的還是惡意的，
        // 而「無法確認」正是黃燈的定義。使用者仍然被警告（findings 逐條列出
        // 未涵蓋的位元組數），只是沒有被告知一件不成立的事。
        QCOMPARE(report.trust, SignatureTrust::Untrusted);

        bool mentionsPartial = false;
        for (const auto& finding : report.findings) {
            if (finding.find("只涵蓋部分檔案") != std::string::npos) mentionsPartial = true;
        }
        QVERIFY2(mentionsPartial, "必須說出『只涵蓋部分檔案』而不是含糊的『無效』");
    }

    void untrustedRootIsYellow() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY(signed_.ok);
        auto file = alioth::test::writeTempPdf(signed_.bytes);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));

        TrustStore empty;  // 不信任任何根憑證
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, empty, options);
        QCOMPARE(reports.size(), std::size_t{1});
        QVERIFY(reports.front().cryptographicallyValid);
        QVERIFY(!reports.front().chainTrusted);
        QCOMPARE(reports.front().trust, SignatureTrust::Untrusted);
    }

    void expiredCertificateIsYellow() {
        const auto signed_ = alioth::test::makeSignedPdf(expired_, &ca_);
        QVERIFY(signed_.ok);
        auto file = alioth::test::writeTempPdf(signed_.bytes);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));

        std::unique_ptr<TrustStore> store(trustingCa());
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const auto reports = scanWith(scanner, *store, options);
        QCOMPARE(reports.size(), std::size_t{1});
        QVERIFY(reports.front().cryptographicallyValid);
        QVERIFY(reports.front().certificateExpired);
        QCOMPARE(reports.front().trust, SignatureTrust::Untrusted);
    }

    void revokedCertificateIsRed() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY(signed_.ok);
        auto file = alioth::test::writeTempPdf(signed_.bytes);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));

        const auto crl = alioth::test::makeCrl(ca_, {1001}, 7 * 24 * 3600);
        QVERIFY(!crl.empty());
        CrlRevocationChecker checker;
        QVERIFY(checker.addCrlDer(crl.data(), crl.size()));

        std::unique_ptr<TrustStore> store(trustingCa());
        const auto reports = scanWith(scanner, *store, VerifyOptions{}, &checker);
        QCOMPARE(reports.size(), std::size_t{1});
        QCOMPARE(reports.front().revocation, RevocationStatus::Revoked);
        QCOMPARE(reports.front().trust, SignatureTrust::Invalid);
    }

    void offlineCrlProvingNotRevokedIsGreen() {
        const auto signed_ = alioth::test::makeSignedPdf(leaf_, &ca_);
        QVERIFY(signed_.ok);
        auto file = alioth::test::writeTempPdf(signed_.bytes);
        QVERIFY(file);
        SignatureScanner scanner;
        QVERIFY(openScanner(scanner, *file));

        const auto crl = alioth::test::makeCrl(ca_, {}, 7 * 24 * 3600);
        CrlRevocationChecker checker;
        QVERIFY(checker.addCrlDer(crl.data(), crl.size()));

        std::unique_ptr<TrustStore> store(trustingCa());
        // 預設政策（SoftFail）下也能是綠燈，因為吊銷狀態是被實際查出來的。
        const auto reports = scanWith(scanner, *store, VerifyOptions{}, &checker);
        QCOMPARE(reports.size(), std::size_t{1});
        QCOMPARE(reports.front().revocation, RevocationStatus::Good);
        QCOMPARE(reports.front().trust, SignatureTrust::Trusted);
    }

    void openingNonPdfFails() {
        QTemporaryFile junk(QStringLiteral("alioth-junk-XXXXXX.pdf"));
        QVERIFY(junk.open());
        junk.write("this is not a pdf at all");
        junk.flush();

        SignatureScanner scanner;
        Latch<DocumentError> latch;
        scanner.open(junk.fileName().toStdString(), {},
                     [&latch](DocumentError error) { latch.set(error); });
        QVERIFY(latch.wait());
        // IL-4：失敗必須明確回報，不得回「成功但沒有簽章」。
        QVERIFY(latch.value() != DocumentError::None);
        QCOMPARE(scanner.signatureCount(), 0);
    }
};

QTEST_APPLESS_MAIN(TestSignatureScanner)
#include "test_signature_scanner.moc"
