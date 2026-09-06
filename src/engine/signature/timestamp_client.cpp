#include "engine/signature/timestamp_client.h"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
#include <openssl/ts.h>
#include <openssl/x509.h>

namespace alioth::engine::signature {

namespace {

// ASN1_GENERALIZEDTIME → Unix 秒。與 pkcs7_verifier.cpp 的 asn1TimeToUnix 是
// 同一招（ASN1_TIME_diff 對 UTCTime 與 GeneralizedTime 一視同仁），這裡沒有
// 直接重用那一份是因為兩個檔案刻意不互相 #include 對方的匿名命名空間細節，
// 兩份十行上下的小函式各自獨立比跨檔案暴露一個內部工具函式的成本低。
[[nodiscard]] std::int64_t generalizedTimeToUnix(const ASN1_GENERALIZEDTIME* time) {
    if (time == nullptr) return 0;
    ASN1_TIME* epoch = ASN1_TIME_set(nullptr, 0);
    if (epoch == nullptr) return 0;
    int days = 0;
    int seconds = 0;
    // ASN1_GENERALIZEDTIME 與 ASN1_TIME 底層都是 ASN1_STRING，OpenSSL 的
    // ASN1_TIME_diff 接受兩者混用，這是官方 apps/ts.c 本身的用法。
    const int ok =
        ASN1_TIME_diff(&days, &seconds, epoch, reinterpret_cast<const ASN1_TIME*>(time));
    ASN1_TIME_free(epoch);
    if (!ok) {
        ERR_clear_error();
        return 0;
    }
    return static_cast<std::int64_t>(days) * 86400 + seconds;
}

[[nodiscard]] std::string serialToHex(const ASN1_INTEGER* serial) {
    if (serial == nullptr) return {};
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
    if (bn == nullptr) {
        ERR_clear_error();
        return {};
    }
    char* text = BN_bn2hex(bn);
    std::string out = text != nullptr ? text : "";
    if (text != nullptr) OPENSSL_free(text);
    BN_free(bn);
    return out;
}

}  // namespace

std::vector<std::uint8_t> buildTimeStampRequestDer(
    const std::vector<std::uint8_t>& messageImprintSha256, const TimestampRequestOptions& options,
    std::string* diagnostic) {
    std::vector<std::uint8_t> out;
    if (messageImprintSha256.empty()) {
        if (diagnostic != nullptr) *diagnostic = "訊息摘要為空";
        return out;
    }

    TS_REQ* request = TS_REQ_new();
    if (request == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "TS_REQ_new 失敗";
        return out;
    }
    TS_REQ_set_version(request, 1);

    TS_MSG_IMPRINT* imprint = TS_MSG_IMPRINT_new();
    X509_ALGOR* algorithm = X509_ALGOR_new();
    bool failed = false;
    std::string failureReason;

    if (imprint == nullptr || algorithm == nullptr) {
        failed = true;
        failureReason = "建立 TS_MSG_IMPRINT／X509_ALGOR 失敗";
    } else {
        // SHA-256 的 AlgorithmIdentifier 參數依慣例是 NULL（V_ASN1_NULL），
        // 而不是省略——部分 TSA 對缺少參數欄位的請求會直接拒絕。
        X509_ALGOR_set0(algorithm, OBJ_nid2obj(NID_sha256), V_ASN1_NULL, nullptr);
        if (TS_MSG_IMPRINT_set_algo(imprint, algorithm) != 1) {
            failed = true;
            failureReason = "TS_MSG_IMPRINT_set_algo 失敗";
        } else if (TS_MSG_IMPRINT_set_msg(
                       imprint, const_cast<unsigned char*>(messageImprintSha256.data()),
                       static_cast<int>(messageImprintSha256.size())) != 1) {
            failed = true;
            failureReason = "TS_MSG_IMPRINT_set_msg 失敗";
        } else if (TS_REQ_set_msg_imprint(request, imprint) != 1) {
            failed = true;
            failureReason = "TS_REQ_set_msg_imprint 失敗";
        }
    }

    if (!failed && TS_REQ_set_cert_req(request, options.requestCertificates ? 1 : 0) != 1) {
        failed = true;
        failureReason = "TS_REQ_set_cert_req 失敗";
    }

    ASN1_INTEGER* nonce = nullptr;
    if (!failed && options.includeNonce) {
        unsigned char nonceBytes[16] = {};
        if (RAND_bytes(nonceBytes, sizeof(nonceBytes)) != 1) {
            failed = true;
            failureReason = "產生亂數失敗";
        } else {
            // 最高位元清零，確保 BN_bin2bn 出來的整數解讀為正值——
            // ASN1_INTEGER 若被誤讀成負數，部分 TSA 會直接拒絕請求。
            nonceBytes[0] &= 0x7F;
            BIGNUM* bn = BN_bin2bn(nonceBytes, static_cast<int>(sizeof(nonceBytes)), nullptr);
            nonce = bn != nullptr ? BN_to_ASN1_INTEGER(bn, nullptr) : nullptr;
            if (bn != nullptr) BN_free(bn);
            if (nonce == nullptr || TS_REQ_set_nonce(request, nonce) != 1) {
                failed = true;
                failureReason = "設定 nonce 失敗";
            }
        }
    }

    if (!failed) {
        unsigned char* buffer = nullptr;
        const int length = i2d_TS_REQ(request, &buffer);
        if (length <= 0 || buffer == nullptr) {
            failed = true;
            failureReason = "序列化 TimeStampReq 失敗";
        } else {
            out.assign(buffer, buffer + length);
            OPENSSL_free(buffer);
        }
    }

    if (nonce != nullptr) ASN1_INTEGER_free(nonce);
    if (imprint != nullptr) TS_MSG_IMPRINT_free(imprint);
    if (algorithm != nullptr) X509_ALGOR_free(algorithm);
    TS_REQ_free(request);

    if (failed) {
        ERR_clear_error();
        if (diagnostic != nullptr) *diagnostic = failureReason;
        out.clear();
    }
    return out;
}

TimestampResult parseTimeStampResponseDer(const std::vector<std::uint8_t>& responseDer) {
    TimestampResult result;
    if (responseDer.empty()) {
        result.diagnostic = "回應為空（傳輸失敗或 TSA 未回應）";
        return result;
    }

    const unsigned char* cursor = responseDer.data();
    TS_RESP* response = d2i_TS_RESP(nullptr, &cursor, static_cast<long>(responseDer.size()));
    if (response == nullptr) {
        ERR_clear_error();
        result.diagnostic = "無法解析 TimeStampResp（不是合法的 DER）";
        return result;
    }

    TS_STATUS_INFO* status = TS_RESP_get_status_info(response);
    const ASN1_INTEGER* statusValue = status != nullptr ? TS_STATUS_INFO_get0_status(status) : nullptr;
    const long statusCode = statusValue != nullptr ? ASN1_INTEGER_get(statusValue) : -1;

    // 只接受「核准」與「核准但有修改」；其餘一律視為沒有拿到可用的時間戳，
    // 即使回應本身格式完全合法也一樣——見標頭說明。
    if (statusCode != TS_STATUS_GRANTED && statusCode != TS_STATUS_GRANTED_WITH_MODS) {
        result.diagnostic = "TSA 未核准請求（status=" + std::to_string(statusCode) + "）";
        TS_RESP_free(response);
        return result;
    }

    PKCS7* token = TS_RESP_get_token(response);
    if (token == nullptr) {
        result.diagnostic = "回應核准但沒有附上 TimeStampToken";
        TS_RESP_free(response);
        return result;
    }

    unsigned char* tokenBuffer = nullptr;
    const int tokenLength = i2d_PKCS7(token, &tokenBuffer);
    if (tokenLength <= 0 || tokenBuffer == nullptr) {
        ERR_clear_error();
        result.diagnostic = "序列化 TimeStampToken 失敗";
        TS_RESP_free(response);
        return result;
    }
    result.tokenDer.assign(tokenBuffer, tokenBuffer + tokenLength);
    OPENSSL_free(tokenBuffer);

    TS_TST_INFO* tstInfo = TS_RESP_get_tst_info(response);
    if (tstInfo != nullptr) {
        result.genTimeUnix = generalizedTimeToUnix(TS_TST_INFO_get_time(tstInfo));
        result.serialHex = serialToHex(TS_TST_INFO_get_serial(tstInfo));
    }

    result.ok = true;
    TS_RESP_free(response);
    return result;
}

TimestampRequester::TimestampRequester(Transport transport) : transport_(std::move(transport)) {}

TimestampResult TimestampRequester::requestToken(const std::string& tsaUrl,
                                                 const std::vector<std::uint8_t>& messageImprintSha256,
                                                 const TimestampRequestOptions& options) const {
    TimestampResult result;
    if (!transport_) {
        result.diagnostic = "尚未設定傳輸層（使用者可能尚未同意連網取得時間戳）";
        return result;
    }

    std::string requestDiagnostic;
    const std::vector<std::uint8_t> requestDer =
        buildTimeStampRequestDer(messageImprintSha256, options, &requestDiagnostic);
    if (requestDer.empty()) {
        result.diagnostic = "建立 TimeStampReq 失敗：" + requestDiagnostic;
        return result;
    }

    const std::vector<std::uint8_t> responseDer = transport_(tsaUrl, requestDer);
    if (responseDer.empty()) {
        result.diagnostic = "傳輸失敗或逾時（未取得任何回應）";
        return result;
    }

    return parseTimeStampResponseDer(responseDer);
}

}  // namespace alioth::engine::signature
