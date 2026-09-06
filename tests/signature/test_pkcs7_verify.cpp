// PKCS#7 驗證、信任存放區、吊銷查詢的測試（WBS 6.6，PRD-SIG-001）。
//
// 這一組用真憑證與真簽章，但不開任何 PDF：驗證核心是純函數，
// 輸入是位元組與 blob，輸出是三態報告。正例與負例都在這裡定義，
// 端到端的 PDF 路徑另見 test_signature_scanner。

#include <QtTest>

#include <algorithm>
#include <climits>

#include "engine/signature/pkcs7_verifier.h"
#include "engine/signature/revocation.h"
#include "engine/signature/trust_store.h"
#include "signature_fixture.h"

using namespace alioth::engine::signature;
using alioth::test::Identity;

namespace {

constexpr long kYear = 365L * 24 * 3600;

std::vector<std::uint8_t> payload(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace

class TestPkcs7Verify : public QObject {
    Q_OBJECT

private:
    Identity ca_;
    Identity leaf_;
    Identity selfSigned_;
    Identity expired_;

private slots:
    void initTestCase() {
        ca_ = alioth::test::makeIdentity("Alioth Test Root CA", nullptr, -kYear, kYear, 1, true);
        QVERIFY(ca_.valid());
        leaf_ = alioth::test::makeIdentity("Mia Legal", &ca_, -3600, kYear, 1001, false);
        QVERIFY(leaf_.valid());
        selfSigned_ =
            alioth::test::makeIdentity("Self Signed Signer", nullptr, -3600, kYear, 2001, false);
        QVERIFY(selfSigned_.valid());
        // notBefore 與 notAfter 都在過去：憑證已過期但簽章本身完全有效。
        expired_ = alioth::test::makeIdentity("Expired Signer", &ca_, -2 * kYear, -kYear, 3001,
                                              false);
        QVERIFY(expired_.valid());
    }

    void emptyContentsIsInvalid() {
        TrustStore store;
        const auto data = payload("hello");
        const SignatureReport report =
            verifyDetachedPkcs7(data, {}, coverageOfWholeInput(static_cast<std::int64_t>(data.size())),
                                store, VerifyOptions{});
        QVERIFY(!report.contentsPresent);
        QCOMPARE(report.trust, SignatureTrust::Invalid);
        QVERIFY(!report.findings.empty());
    }

    void garbageBlobIsInvalidNotCrash() {
        TrustStore store;
        const auto data = payload("hello");
        const std::vector<std::uint8_t> garbage{0x30, 0x82, 0xFF, 0xFF, 0x01, 0x02, 0x03};
        const SignatureReport report =
            verifyDetachedPkcs7(data, garbage,
                                coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store,
                                VerifyOptions{});
        QVERIFY(!report.parsed);
        QCOMPARE(report.trust, SignatureTrust::Invalid);
    }

    void trustedChainWithSkippedRevocationIsGreen() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        QVERIFY(!der.empty());

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        QCOMPARE(store.size(), std::size_t{1});

        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store, options);

        QVERIFY(report.parsed);
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.chainTrusted);
        QVERIFY(!report.certificateExpired);
        QCOMPARE(report.signerName, std::string("Mia Legal"));
        QVERIFY(report.chain.size() >= 2);
        QCOMPARE(report.trust, SignatureTrust::Trusted);
    }

    void untrustedRootIsYellowNotGreen() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(selfSigned_, data, nullptr);
        QVERIFY(!der.empty());

        TrustStore store;  // 刻意留空：沒有任何信任錨
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store, options);

        // 簽章數學上完全正確，只是沒有人替簽署者背書。
        QVERIFY(report.digestMatches);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(!report.chainTrusted);
        QVERIFY(report.chain.size() >= 1);
        QVERIFY(report.chain.front().selfSigned);
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
    }

    void tamperedPayloadIsRed() {
        auto data = payload("contract body v1 with enough bytes to matter");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        QVERIFY(!der.empty());

        // 改一個位元組。這是竄改偵測的最小案例，也是最重要的一個。
        data[10] ^= 0x01;

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store, options);

        QVERIFY(report.parsed);
        QVERIFY(!report.cryptographicallyValid);
        QVERIFY(!report.digestMatches);
        QCOMPARE(report.trust, SignatureTrust::Invalid);
    }

    void expiredCertificateIsYellowWithExplicitReason() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(expired_, data, &ca_);
        QVERIFY(!der.empty());

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;
        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store, options);

        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.certificateExpired);
        // 沒有可信時間戳就無法證明簽署當下憑證仍有效，因此是黃燈而不是綠燈；
        // 但也不是紅燈——簽章本身沒有問題。
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
        bool mentionsExpiry = false;
        for (const auto& finding : report.findings) {
            if (finding.find("過期") != std::string::npos) mentionsExpiry = true;
        }
        QVERIFY(mentionsExpiry);
    }

    // 密碼學完美但涵蓋不完整 → 黃燈「無法確認」，不是紅燈「無效」。
    // 完整理由見 exceptions/EXC_20260906_RD_SA_sig003_trust_gap.md 的裁決：
    // 判紅燈等於讓每一份被自己編輯過的簽章文件都顯示無效，而那是對本產品
    // 核心迴圈（加註解、增量儲存）的誤報。
    void incompleteCoverageIsAmberEvenWithPerfectSignature() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        QVERIFY(!der.empty());

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;

        // 宣稱檔案比實際被簽的位元組長：尾端有內容不在簽章涵蓋範圍內。
        ByteRangeCheck partial =
            checkByteRange({0, static_cast<int>(data.size())},
                           static_cast<std::int64_t>(data.size()) + 500);
        QVERIFY(partial.partiallyCovered());

        const SignatureReport report =
            verifyDetachedPkcs7(data, der, partial, store, options);
        QVERIFY(report.cryptographicallyValid);
        QVERIFY(report.coverage.partiallyCovered());
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
    }

    void crlSaysNotRevokedThenGreen() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        const auto crl = alioth::test::makeCrl(ca_, {}, 7 * 24 * 3600);
        QVERIFY(!crl.empty());

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        CrlRevocationChecker checker;
        QVERIFY(checker.addCrlDer(crl.data(), crl.size()));

        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store,
            VerifyOptions{}, &checker);
        QCOMPARE(report.revocation, RevocationStatus::Good);
        QCOMPARE(report.trust, SignatureTrust::Trusted);
    }

    void crlListingSerialMakesItRed() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        const auto crl = alioth::test::makeCrl(ca_, {1001}, 7 * 24 * 3600);
        QVERIFY(!crl.empty());

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        CrlRevocationChecker checker;
        QVERIFY(checker.addCrlDer(crl.data(), crl.size()));

        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store,
            VerifyOptions{}, &checker);
        QCOMPARE(report.revocation, RevocationStatus::Revoked);
        QCOMPARE(report.trust, SignatureTrust::Invalid);
    }

    void unrelatedCrlDoesNotCountAsProof() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        // 由另一個 CA 簽發的 CRL。若拿它當作「未被吊銷」的證據，
        // 任何人都能自己簽一份 CRL 來蓋掉真正的吊銷紀錄。
        Identity otherCa =
            alioth::test::makeIdentity("Other CA", nullptr, -kYear, kYear, 9, true);
        QVERIFY(otherCa.valid());
        const auto crl = alioth::test::makeCrl(otherCa, {}, 7 * 24 * 3600);

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        CrlRevocationChecker checker;
        QVERIFY(checker.addCrlDer(crl.data(), crl.size()));

        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store,
            VerifyOptions{}, &checker);
        QCOMPARE(report.revocation, RevocationStatus::Unknown);
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
    }

    void defaultPolicyDowngradesWhenRevocationUnchecked() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));

        // 預設政策是 SoftFail：不給查詢器就只能是黃燈。
        const SignatureReport report = verifyDetachedPkcs7(
            data, der, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store,
            VerifyOptions{});
        QVERIFY(report.chainTrusted);
        QCOMPARE(report.revocation, RevocationStatus::NotChecked);
        QCOMPARE(report.trust, SignatureTrust::Untrusted);
    }

    void ocspWithoutTransportIsUnknownNotGood() {
        // 沒有傳輸就查不到。回 Good 會讓離線環境永遠亮綠燈。
        OcspRevocationChecker checker;
        QCOMPARE(checker.check(leaf_.cert, ca_.cert, 0), RevocationStatus::Unknown);
        // 憑證沒有 AIA 擴充，責任者 URL 應為空。
        QVERIFY(OcspRevocationChecker::responderUrl(leaf_.cert).empty());

        bool called = false;
        OcspRevocationChecker withTransport(
            [&called](const std::string&, const std::vector<std::uint8_t>&) {
                called = true;
                return std::vector<std::uint8_t>{};
            });
        QCOMPARE(withTransport.check(leaf_.cert, ca_.cert, 0), RevocationStatus::Unknown);
        // 沒有 URL 就不該產生任何對外請求。
        QVERIFY(!called);
    }

    // size_t → long/int 轉型截斷的迴歸測試（F-001/F-002）：pkcs7Der 與
    // signedBytes 都來自不可信的 PDF 位元組，超過 OpenSSL API 能收的長度上限
    // 時必須明確拒絕，不能靜默截斷後誤判驗證結果。這裡真的配置超過
    // INT_MAX 位元組來釘住邊界，而不是只檢查函式簽章。
    void oversizedSignedBytesIsRejectedNotTruncated() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        QVERIFY(!der.empty());

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions options;
        options.revocationPolicy = RevocationPolicy::Skip;

        std::vector<std::uint8_t> oversized;
        oversized.resize(static_cast<std::size_t>(INT_MAX) + 1, 0);
        // 把真正的簽署內容放在開頭：如果實作不小心把長度截斷成 int，
        // PKCS7_verify 仍然可能「碰巧」在截斷後的資料上算出摘要相符，
        // 讓這個測試看起來像誤判通過而不是真的截斷了。
        std::copy(data.begin(), data.end(), oversized.begin());

        const SignatureReport report = verifyDetachedPkcs7(
            oversized, der, coverageOfWholeInput(static_cast<std::int64_t>(oversized.size())),
            store, options);

        QVERIFY(report.parsed);
        QVERIFY(!report.digestMatches);
        QVERIFY(!report.cryptographicallyValid);
        QCOMPARE(report.trust, SignatureTrust::Invalid);
        bool mentionsLimit = false;
        for (const auto& finding : report.findings) {
            if (finding.find("上限") != std::string::npos) mentionsLimit = true;
        }
        QVERIFY(mentionsLimit);
    }

    void oversizedPkcs7DerIsRejectedNotTruncated() {
        // long 的寬度是 ABI 決定的：Windows（LLP64）是 32 位元，所以 size_t
        // 真的可以超過 LONG_MAX，這條邊界必須釘住。Linux / macOS（LP64）
        // 的 long 與 size_t 同寬，那條路徑不可能被觸發，而
        // static_cast<std::size_t>(LONG_MAX) + 1 會變成 2^63——resize 到那個
        // 大小會擲出 length_error，測試行程直接 terminate，看起來像崩潰而不是
        // 「這個平台沒有這個邊界」。所以在那些平台明確跳過而不是嘗試配置。
        if constexpr (sizeof(long) >= sizeof(std::size_t)) {  // NOLINT(google-runtime-int)
            QSKIP("long 與 size_t 同寬：pkcs7Der.size() 不可能超過 LONG_MAX");
        }

        const auto data = payload("hello");
        std::vector<std::uint8_t> oversizedDer;
        oversizedDer.resize(static_cast<std::size_t>(LONG_MAX) + 1, 0);

        TrustStore store;
        const SignatureReport report = verifyDetachedPkcs7(
            data, oversizedDer, coverageOfWholeInput(static_cast<std::int64_t>(data.size())), store,
            VerifyOptions{});

        QVERIFY(!report.parsed);
        QCOMPARE(report.trust, SignatureTrust::Invalid);
        bool mentionsLimit = false;
        for (const auto& finding : report.findings) {
            if (finding.find("上限") != std::string::npos) mentionsLimit = true;
        }
        QVERIFY(mentionsLimit);
    }

    void trimDerPaddingRemovesTrailingZeros() {
        const auto data = payload("contract body v1");
        const auto der = alioth::test::signDetached(leaf_, data, &ca_);
        QVERIFY(!der.empty());

        std::vector<std::uint8_t> padded = der;
        padded.resize(der.size() + 4096, 0x00);
        const auto trimmed = trimDerPadding(padded);
        QCOMPARE(trimmed.size(), der.size());
        QVERIFY(trimmed == der);

        // 已經剛好的資料不能被截短。
        QCOMPARE(trimDerPadding(der).size(), der.size());
    }
};

QTEST_APPLESS_MAIN(TestPkcs7Verify)
#include "test_pkcs7_verify.moc"
