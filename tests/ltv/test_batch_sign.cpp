// 批次簽署共同基礎的測試（PRD-SIG-005「多頁批次簽章」，語意未定案，見
// exceptions/EXC_20260906_RD_SA_wp38_sig005_semantics.md）。
//
// 兩個測試對應兩種可能的語意解讀，證明 signBatch() 不需要為了任一種解讀
// 改動自己：呼叫端只是餵不同的 BatchSignItem 清單。哪一種才是 PRD 真正要的
// 語意仍然 BLOCKED，這裡不代表語意已經裁定。

#include <QtTest>

#include <openssl/bio.h>
#include <openssl/pem.h>

#include "engine/signature/signature_creator.h"
#include "pdf_fixture.h"
#include "signature_fixture.h"

using namespace alioth::engine::signature;
using alioth::test::Identity;

namespace {
constexpr long kYear = 365L * 24 * 3600;
}  // namespace

class TestBatchSign : public QObject {
    Q_OBJECT

private:
    Identity ca_;
    Identity leaf_;

    void fillSigningIdentity(SigningIdentity& identity) {
        [&] { QVERIFY(identity.loadPem(alioth::test::certificatePem(leaf_), privateKeyPem())); }();
        [&] { QVERIFY(identity.addChainCertificatePem(alioth::test::certificatePem(ca_))); }();
    }

    [[nodiscard]] std::string privateKeyPem() const {
        BIO* bio = BIO_new(BIO_s_mem());
        if (!bio) return {};
        PEM_write_bio_PrivateKey(bio, leaf_.key, nullptr, nullptr, 0, nullptr, nullptr);
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio, &data);
        std::string out;
        if (length > 0 && data) out.assign(data, static_cast<std::size_t>(length));
        BIO_free(bio);
        return out;
    }

private slots:
    void initTestCase() {
        ca_ = alioth::test::makeIdentity("Alioth Test Root CA", nullptr, -kYear, kYear, 1, true);
        QVERIFY(ca_.valid());
        leaf_ = alioth::test::makeIdentity("Batch Signer", &ca_, -3600, kYear, 1003, false);
        QVERIFY(leaf_.valid());
    }

    // 解讀 1：對多份不同文件各簽一次。
    void signsMultipleIndependentDocuments() {
        SigningIdentity identity;
        fillSigningIdentity(identity);

        std::vector<BatchSignItem> items;
        for (int i = 0; i < 3; ++i) {
            BatchSignItem item;
            item.sourceBytes = alioth::test::makeSinglePagePdf().toStdString();
            item.options.reason = "batch doc " + std::to_string(i);
            item.options.signingTimeUnix = 1735732800;
            items.push_back(std::move(item));
        }

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions verifyOptions;
        verifyOptions.revocationPolicy = RevocationPolicy::Skip;

        const BatchSignResult result = signBatch(identity, items, &store, verifyOptions);
        QCOMPARE(result.items.size(), std::size_t{3});
        QCOMPARE(result.successCount(), std::size_t{3});
        for (const BatchSignItemResult& item : result.items) {
            QVERIFY2(item.create.ok, item.create.diagnostic.c_str());
            QVERIFY(item.verifyAttempted);
            QCOMPARE(item.verify.trust, SignatureTrust::Trusted);
        }
    }

    // 解讀 2：同一份文件的多個簽章欄位依序簽署——上一輪的輸出是下一輪的輸入，
    // 且每一輪都必須用不同的 /T 欄位名稱（AcroForm 欄位名稱唯一性）。
    void signsTheSameDocumentAcrossMultipleFieldsSequentially() {
        SigningIdentity identity;
        fillSigningIdentity(identity);

        const std::string original = alioth::test::makeSinglePagePdf().toStdString();

        BatchSignItem first;
        first.sourceBytes = original;
        first.options.fieldName = "Signature1";
        first.options.signingTimeUnix = 1735732800;

        CreateSignatureResult firstResult = createSignature(first.sourceBytes, identity, first.options);
        QVERIFY2(firstResult.ok, firstResult.diagnostic.c_str());

        BatchSignItem second;
        second.sourceBytes = firstResult.bytes;  // 接力：這一輪的輸入是上一輪的輸出
        second.options.fieldName = "Signature2";
        second.options.documentAlreadyHasSignatures = true;
        second.options.signingTimeUnix = 1735732900;

        std::vector<BatchSignItem> items{std::move(second)};

        TrustStore store;
        QVERIFY(store.addCertificatePem(alioth::test::certificatePem(ca_)));
        VerifyOptions verifyOptions;
        verifyOptions.revocationPolicy = RevocationPolicy::Skip;

        const BatchSignResult result = signBatch(identity, items, &store, verifyOptions);
        QCOMPARE(result.successCount(), std::size_t{1});
        QVERIFY(result.items[0].verify.trust == SignatureTrust::Trusted);

        // 最終輸出裡兩個欄位都要在——第二次簽署不能把第一次的簽章擠掉。
        const std::string& finalBytes = result.items[0].create.bytes;
        QVERIFY(finalBytes.find("Signature1") != std::string::npos);
        QVERIFY(finalBytes.find("Signature2") != std::string::npos);
    }

    void withoutTrustStoreSkipsVerification() {
        SigningIdentity identity;
        fillSigningIdentity(identity);

        BatchSignItem item;
        item.sourceBytes = alioth::test::makeSinglePagePdf().toStdString();
        const BatchSignResult result = signBatch(identity, {item}, nullptr);
        QCOMPARE(result.items.size(), std::size_t{1});
        QVERIFY(result.items[0].create.ok);
        QVERIFY(!result.items[0].verifyAttempted);
    }
};

QTEST_APPLESS_MAIN(TestBatchSign)
#include "test_batch_sign.moc"
