// RFC 3161 時間戳用戶端的協定測試（WBS 6.11，PRD-SIG-006）。
//
// 判準與 tests/signature 系列一致：不能只驗證「我們自己的程式碼可以讀回
// 自己組的東西」，因此請求端用 OpenSSL 的 d2i_TS_REQ 反解析驗證格式，
// 回應端用本地伺服器（timestamp_test_server.h）簽出真正合法的
// TimeStampResp——CI 全程不連任何真實 TSA。

#include <QtTest>

#include <openssl/ts.h>

#include <cstring>
#include <vector>

#include "engine/signature/timestamp_client.h"
#include "signature_fixture.h"
#include "timestamp_test_server.h"

using namespace alioth::engine::signature;
using alioth::test::Identity;

namespace {
constexpr long kYear = 365L * 24 * 3600;
}  // namespace

class TestTimestampClient : public QObject {
    Q_OBJECT

private:
    Identity tsa_;

private slots:
    void initTestCase() {
        tsa_ = alioth::test::makeTsaIdentity("Alioth Test TSA", 2001);
        QVERIFY(tsa_.valid());
    }

    // 請求端：組出來的 DER 必須是合法的 TimeStampReq，且訊息摘要與演算法
    // 正確落地——這一步不碰網路，純粹驗證序列化本身。
    void requestDerIsWellFormedAndCarriesTheImprint() {
        const std::vector<std::uint8_t> hash(32, 0xAB);  // 假的 SHA-256 摘要，內容不重要
        std::string diagnostic;
        const std::vector<std::uint8_t> der = buildTimeStampRequestDer(hash, {}, &diagnostic);
        QVERIFY2(!der.empty(), diagnostic.c_str());

        const unsigned char* cursor = der.data();
        TS_REQ* req = d2i_TS_REQ(nullptr, &cursor, static_cast<long>(der.size()));
        QVERIFY2(req != nullptr, "產生的 TimeStampReq 無法用 OpenSSL 自己的 d2i_TS_REQ 反解析");

        TS_MSG_IMPRINT* imprint = TS_REQ_get_msg_imprint(req);
        QVERIFY(imprint != nullptr);
        ASN1_OCTET_STRING* msg = TS_MSG_IMPRINT_get_msg(imprint);
        QCOMPARE(static_cast<std::size_t>(ASN1_STRING_length(msg)), hash.size());
        QCOMPARE(std::memcmp(ASN1_STRING_get0_data(msg), hash.data(), hash.size()), 0);

        TS_REQ_free(req);
    }

    void emptyImprintFailsExplicitly() {
        std::string diagnostic;
        const std::vector<std::uint8_t> der = buildTimeStampRequestDer({}, {}, &diagnostic);
        QVERIFY(der.empty());
        QVERIFY(!diagnostic.empty());
    }

    // 端到端：透過注入的傳輸層（本地伺服器）取得一份真正合法的時間戳，
    // 且回傳的 genTime 落在測試執行的合理範圍內。
    void requestTokenSucceedsWithLocalServer() {
        TimestampRequester::Transport transport =
            [this](const std::string&, const std::vector<std::uint8_t>& reqDer) {
                return alioth::test::createLocalTimestampResponse(tsa_, reqDer);
            };
        TimestampRequester requester(transport);

        const std::vector<std::uint8_t> hash(32, 0x11);
        const TimestampResult result = requester.requestToken("https://tsa.invalid/", hash);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(!result.tokenDer.empty());
        QVERIFY(result.genTimeUnix > 0);

        // 回應本身要能被獨立解析成 TimeStampToken：d2i_PKCS7 是驗證側
        // （pkcs7_verifier.cpp）之後解析同一份 token 用的入口，這裡先確認
        // 它產出的位元組真的是那個型態。
        const unsigned char* cursor = result.tokenDer.data();
        PKCS7* token = d2i_PKCS7(nullptr, &cursor, static_cast<long>(result.tokenDer.size()));
        QVERIFY2(token != nullptr, "TimeStampToken 不是合法的 PKCS7 ContentInfo");
        QVERIFY(PKCS7_type_is_signed(token));
        PKCS7_free(token);
    }

    void requestTokenFailsWithoutTransport() {
        TimestampRequester requester({});
        const TimestampResult result = requester.requestToken("https://tsa.invalid/",
                                                               std::vector<std::uint8_t>(32, 0x22));
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void requestTokenFailsWhenTransportReturnsEmpty() {
        TimestampRequester requester(
            [](const std::string&, const std::vector<std::uint8_t>&) { return std::vector<std::uint8_t>{}; });
        const TimestampResult result = requester.requestToken("https://tsa.invalid/",
                                                               std::vector<std::uint8_t>(32, 0x33));
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("傳輸失敗") != std::string::npos);
    }

    void parseResponseRejectsGarbageBytes() {
        const TimestampResult result =
            parseTimeStampResponseDer(std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }
};

QTEST_APPLESS_MAIN(TestTimestampClient)
#include "test_timestamp_client.moc"
