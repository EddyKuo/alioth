#pragma once

// 本地時間戳伺服器（僅供測試）。
//
// CI 不能依賴網路，因此 timestamp_client.h 的 Transport 一律注入這裡產生的
// 位元組，不連任何真實 TSA。做法是用 OpenSSL 的伺服端 API（TS_RESP_CTX /
// TS_RESP_create_response）把用戶端組出的 TimeStampReq 就地簽出一份合法的
// TimeStampResp——這與 tests/signature/signature_fixture.h 手工造簽章語料
// 是同一個精神：有了可控的正例，才能證明負例真的是因為協定不合規而失敗，
// 不是我們自己的解析從頭到尾都是錯的。
//
// 這個檔案只給測試用，刻意不放進 src/：正式程式碼永遠不該內建一個「自己
// 簽發時間戳」的能力，那與「TSA 是獨立第三方見證」的信任模型直接衝突。

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <string>
#include <vector>

#include "signature_fixture.h"  // tests/signature/signature_fixture.h 的 Identity / makeIdentity

namespace alioth::test {

// 任意選一個政策 OID：RFC 3161 要求回應帶政策識別碼，但本測試伺服器不實作
// 真正的政策語意，隨便一個合法的 OID 字串即可。
inline constexpr const char* kTestTsaPolicyOid = "1.2.3.4.5.6.7.8.9.1";

// 專用的 TSA 憑證產生器：不能直接借用 signature_fixture.h 的 makeIdentity，
// 因為那邊固定寫死 ext_key_usage=emailProtection（給一般簽署者用）。RFC 3161
// 要求 TSA 憑證的 Extended Key Usage **只能**是 id-kp-timeStamping 且標記
// critical——OpenSSL 的 TS_RESP_CTX_set_signer_cert 會就地核對這一點，
// 少了它或多了別的用途都會讓 set_signer_cert 靜默回傳失敗，症狀是
// createLocalTimestampResponse 回傳空向量，看起來像協定本身有問題，
// 其實只是測試憑證的 EKU 選錯。
[[nodiscard]] inline Identity makeTsaIdentity(const std::string& commonName, long serial) {
    Identity identity;
    identity.key = EVP_RSA_gen(2048);
    if (!identity.key) return identity;
    identity.cert = X509_new();
    if (!identity.cert) return identity;

    X509_set_version(identity.cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(identity.cert), serial);
    X509_gmtime_adj(X509_getm_notBefore(identity.cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(identity.cert), 365L * 24 * 3600);
    X509_set_pubkey(identity.cert, identity.key);

    X509_NAME* subject = X509_get_subject_name(identity.cert);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1, 0);
    X509_set_issuer_name(identity.cert, subject);  // 自簽，測試不需要獨立的 TSA CA

    setExtension(identity.cert, identity.cert, NID_basic_constraints, "critical,CA:FALSE");
    setExtension(identity.cert, identity.cert, NID_key_usage, "critical,digitalSignature");
    setExtension(identity.cert, identity.cert, NID_ext_key_usage, "critical,timeStamping");
    setExtension(identity.cert, identity.cert, NID_subject_key_identifier, "hash");

    if (X509_sign(identity.cert, identity.key, EVP_sha256()) == 0) {
        ERR_clear_error();
        X509_free(identity.cert);
        identity.cert = nullptr;
    }
    return identity;
}

// 用一組（可能是自簽的）身分產生時間戳回應。回傳空向量代表產生失敗
// （通常是請求本身不合法的 DER，或是身分無效）。
[[nodiscard]] inline std::vector<std::uint8_t> createLocalTimestampResponse(
    const Identity& tsa, const std::vector<std::uint8_t>& requestDer) {
    std::vector<std::uint8_t> out;
    if (!tsa.valid() || requestDer.empty()) return out;

    TS_RESP_CTX* ctx = TS_RESP_CTX_new();
    if (ctx == nullptr) return out;

    bool ok = true;
    ASN1_OBJECT* policy = OBJ_txt2obj(kTestTsaPolicyOid, 1);
    ok = ok && policy != nullptr;
    ok = ok && TS_RESP_CTX_set_signer_cert(ctx, tsa.cert) == 1;
    ok = ok && TS_RESP_CTX_set_signer_key(ctx, tsa.key) == 1;
    ok = ok && TS_RESP_CTX_set_signer_digest(ctx, EVP_sha256()) == 1;
    ok = ok && TS_RESP_CTX_set_def_policy(ctx, policy) == 1;
    ok = ok && TS_RESP_CTX_add_md(ctx, EVP_sha256()) == 1;
    // 精確度留給預設值（不設定）；本測試不驗證這個欄位。
    if (policy != nullptr) ASN1_OBJECT_free(policy);

    if (ok) {
        BIO* requestBio = BIO_new_mem_buf(requestDer.data(), static_cast<int>(requestDer.size()));
        if (requestBio != nullptr) {
            TS_RESP* response = TS_RESP_create_response(ctx, requestBio);
            BIO_free(requestBio);
            if (response != nullptr) {
                unsigned char* buffer = nullptr;
                const int length = i2d_TS_RESP(response, &buffer);
                if (length > 0 && buffer != nullptr) {
                    out.assign(buffer, buffer + length);
                    OPENSSL_free(buffer);
                }
                TS_RESP_free(response);
            }
        }
    }

    TS_RESP_CTX_free(ctx);
    ERR_clear_error();
    return out;
}

}  // namespace alioth::test
