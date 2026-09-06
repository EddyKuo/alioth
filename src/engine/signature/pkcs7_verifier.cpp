#include "engine/signature/pkcs7_verifier.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <cstring>
#include <ctime>

namespace alioth::engine::signature {
namespace {

std::string nameEntry(X509_NAME* name, int nid) {
    if (!name) return {};
    const int index = X509_NAME_get_index_by_NID(name, nid, -1);
    if (index < 0) return {};
    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, index);
    if (!entry) return {};
    ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
    if (!data) return {};
    unsigned char* utf8 = nullptr;
    const int length = ASN1_STRING_to_UTF8(&utf8, data);
    if (length < 0 || !utf8) {
        ERR_clear_error();
        return {};
    }
    std::string out(reinterpret_cast<char*>(utf8), static_cast<std::size_t>(length));
    OPENSSL_free(utf8);
    return out;
}

std::string oneLineName(X509_NAME* name) {
    if (!name) return {};
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return {};
    // XN_FLAG_RFC2253 之外加上 ASN1_STRFLGS_UTF8_CONVERT，否則中文主體名會
    // 被輸出成 \xE5\xBC\xB5 這種轉義序列，直接顯示在面板上是不可讀的。
    X509_NAME_print_ex(bio, name, 0,
                       (XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) | ASN1_STRFLGS_UTF8_CONVERT);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string out;
    if (length > 0 && data) out.assign(data, static_cast<std::size_t>(length));
    BIO_free(bio);
    return out;
}

// ASN1_TIME → Unix 秒。刻意不用 timegm / _mkgmtime：那組介面在各平台名稱不同，
// 而作業系統差異只准出現在平台層。ASN1_TIME_diff 是 OpenSSL 自帶的可攜路徑。
std::int64_t asn1TimeToUnix(const ASN1_TIME* time) {
    if (!time) return 0;
    ASN1_TIME* epoch = ASN1_TIME_set(nullptr, 0);
    if (!epoch) return 0;
    int days = 0;
    int seconds = 0;
    const int ok = ASN1_TIME_diff(&days, &seconds, epoch, time);
    ASN1_TIME_free(epoch);
    if (!ok) {
        ERR_clear_error();
        return 0;
    }
    return static_cast<std::int64_t>(days) * 86400 + seconds;
}

std::string serialHex(X509* cert) {
    if (!cert) return {};
    ASN1_INTEGER* serial = X509_get_serialNumber(cert);
    if (!serial) return {};
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
    if (!bn) {
        ERR_clear_error();
        return {};
    }
    char* text = BN_bn2hex(bn);
    std::string out = text ? text : "";
    if (text) OPENSSL_free(text);
    BN_free(bn);
    return out;
}

std::string sha256Fingerprint(X509* cert) {
    if (!cert) return {};
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int length = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
        ERR_clear_error();
        return {};
    }
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 3);
    for (unsigned int i = 0; i < length; ++i) {
        if (i > 0) out.push_back(':');
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

CertificateInfo describeCertificate(X509* cert) {
    CertificateInfo info;
    if (!cert) return info;
    X509_NAME* subject = X509_get_subject_name(cert);
    X509_NAME* issuer = X509_get_issuer_name(cert);
    info.subject = oneLineName(subject);
    info.issuer = oneLineName(issuer);
    info.serialHex = serialHex(cert);
    info.sha256Fingerprint = sha256Fingerprint(cert);
    info.notBefore = asn1TimeToUnix(X509_get0_notBefore(cert));
    info.notAfter = asn1TimeToUnix(X509_get0_notAfter(cert));
    info.selfSigned = subject && issuer && X509_NAME_cmp(subject, issuer) == 0;
    return info;
}

// 清空錯誤佇列，並回報其中是否出現「摘要不符」。
//
// 用來分辨「檔案被改過」與「其他驗證失敗」（例如演算法不支援）——
// 兩者都讓 PKCS7_verify 回傳 0，但對使用者的意義完全不同。
//
// 必須掃整個佇列而不是只看最後一筆：OpenSSL 在偵測到摘要不符之後還會
// 再推入一筆較籠統的錯誤，只看最後一筆會永遠看不到真正的原因。
// 從 unauthenticated attribute 掛的 id-aa-signatureTimeStampToken 取出
// TSTInfo 的 genTime。找不到、解析失敗，或屬性乾脆不存在，一律回傳 false
// 且不改動 genTime——「沒有時間戳」與「時間戳解析失敗」對使用者來說都是
// 「不能拿它當 PAdES-T 用」，這裡不強求區分兩者，錯誤細節不影響任何判色。
[[nodiscard]] bool extractSignatureTimestamp(PKCS7_SIGNER_INFO* signerInfo,
                                             std::int64_t& genTimeUnixOut) {
    STACK_OF(X509_ATTRIBUTE)* unauthAttrs = PKCS7_get_attributes(signerInfo);
    if (unauthAttrs == nullptr) return false;
    const int index = X509at_get_attr_by_NID(unauthAttrs, NID_id_smime_aa_timeStampToken, -1);
    if (index < 0) return false;
    X509_ATTRIBUTE* attribute = X509at_get_attr(unauthAttrs, index);
    if (attribute == nullptr) return false;

    auto* tokenString = static_cast<ASN1_STRING*>(
        X509_ATTRIBUTE_get0_data(attribute, 0, V_ASN1_SEQUENCE, nullptr));
    if (tokenString == nullptr) {
        ERR_clear_error();
        return false;
    }

    const unsigned char* cursor = ASN1_STRING_get0_data(tokenString);
    PKCS7* token = d2i_PKCS7(nullptr, &cursor, ASN1_STRING_length(tokenString));
    if (token == nullptr) {
        ERR_clear_error();
        return false;
    }

    TS_TST_INFO* tstInfo = PKCS7_to_TS_TST_INFO(token);
    PKCS7_free(token);
    if (tstInfo == nullptr) {
        ERR_clear_error();
        return false;
    }

    genTimeUnixOut = asn1TimeToUnix(TS_TST_INFO_get_time(tstInfo));
    TS_TST_INFO_free(tstInfo);
    return true;
}

bool drainAndSawDigestFailure() {
    bool sawDigestFailure = false;
    while (const unsigned long code = ERR_get_error()) {
        if (ERR_GET_LIB(code) == ERR_LIB_PKCS7 &&
            ERR_GET_REASON(code) == PKCS7_R_DIGEST_FAILURE) {
            sawDigestFailure = true;
        }
    }
    return sawDigestFailure;
}

}  // namespace

std::vector<std::uint8_t> trimDerPadding(const std::vector<std::uint8_t>& contents) {
    if (contents.size() < 2) return contents;
    if (contents[0] != 0x30) return contents;  // 不是 SEQUENCE，交給解析器自己失敗

    std::size_t headerLength = 2;
    std::size_t bodyLength = contents[1];
    if (bodyLength & 0x80u) {
        const std::size_t lengthBytes = bodyLength & 0x7Fu;
        // 長度欄位本身超過 8 位元組，或宣稱的位元組數超出實際資料，都是壞資料。
        if (lengthBytes == 0 || lengthBytes > 8 || contents.size() < 2 + lengthBytes) {
            return contents;
        }
        bodyLength = 0;
        for (std::size_t i = 0; i < lengthBytes; ++i) {
            bodyLength = (bodyLength << 8) | contents[2 + i];
        }
        headerLength = 2 + lengthBytes;
    }

    const std::size_t total = headerLength + bodyLength;
    if (total > contents.size()) return contents;
    return std::vector<std::uint8_t>(contents.begin(),
                                     contents.begin() + static_cast<std::ptrdiff_t>(total));
}

SignatureReport verifyDetachedPkcs7(const std::vector<std::uint8_t>& signedBytes,
                                    const std::vector<std::uint8_t>& pkcs7Der,
                                    const ByteRangeCheck& coverage, const TrustStore& trust,
                                    const VerifyOptions& options, RevocationChecker* revocation) {
    SignatureReport report;
    report.coverage = coverage;
    report.contentsPresent = !pkcs7Der.empty();
    if (!report.contentsPresent) {
        finalize(report, options.revocationPolicy);
        return report;
    }

    const unsigned char* cursor = pkcs7Der.data();
    PKCS7* p7 = d2i_PKCS7(nullptr, &cursor, static_cast<long>(pkcs7Der.size()));
    if (!p7) {
        ERR_clear_error();
        finalize(report, options.revocationPolicy);
        return report;
    }
    if (!PKCS7_type_is_signed(p7)) {
        PKCS7_free(p7);
        finalize(report, options.revocationPolicy);
        return report;
    }
    report.parsed = true;

    auto* store = static_cast<X509_STORE*>(trust.nativeHandle());

    // 被簽的內容為空代表 /ByteRange 檢查已經判定沒有可驗的位元組。
    // 這種情況下不能宣稱驗證通過——直接留在 Invalid。
    if (!signedBytes.empty() && store) {
        BIO* data = BIO_new_mem_buf(signedBytes.data(), static_cast<int>(signedBytes.size()));
        if (data) {
            // 第一次：連憑證鏈一起驗。成功代表綠燈的前置條件全部滿足。
            if (PKCS7_verify(p7, nullptr, store, data, nullptr, 0) == 1) {
                report.digestMatches = true;
                report.cryptographicallyValid = true;
                report.chainTrusted = true;
            } else {
                ERR_clear_error();
                BIO_free(data);
                data = BIO_new_mem_buf(signedBytes.data(), static_cast<int>(signedBytes.size()));
                // 第二次：跳過憑證鏈，只驗簽章本身。
                // 這一步是「有效但不受信任」與「根本無效」的分界線，
                // 沒有它就無法把自簽憑證正確地歸到黃燈而不是紅燈。
                if (data && PKCS7_verify(p7, nullptr, store, data, nullptr, PKCS7_NOVERIFY) == 1) {
                    report.digestMatches = true;
                    report.cryptographicallyValid = true;
                    report.chainTrusted = false;
                } else {
                    report.digestMatches = !drainAndSawDigestFailure();
                    report.cryptographicallyValid = false;
                }
            }
            if (data) BIO_free(data);
        }
    }

    // 憑證資訊要在驗證成敗兩種情況下都取得：使用者看到紅燈時仍然需要知道是誰簽的。
    X509* signer = nullptr;
    if (STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, PKCS7_NOVERIFY)) {
        if (sk_X509_num(signers) > 0) signer = sk_X509_value(signers, 0);
        sk_X509_free(signers);
    }
    ERR_clear_error();

    if (signer) {
        report.chain.push_back(describeCertificate(signer));
        report.signerName = nameEntry(X509_get_subject_name(signer), NID_commonName);
        if (report.signerName.empty()) report.signerName = report.chain.front().subject;

        // X509_cmp_time 的第二個參數不是 const，所以這裡不能宣告成 const。
        std::time_t now = options.verificationTime > 0
                              ? static_cast<std::time_t>(options.verificationTime)
                              : std::time(nullptr);
        report.certificateNotYetValid = X509_cmp_time(X509_get0_notBefore(signer), &now) > 0;
        report.certificateExpired = X509_cmp_time(X509_get0_notAfter(signer), &now) < 0;
    }

    // PRD-SIG-006：偵測 unsigned attribute 掛的時間戳權杖。只取第一個
    // signer info——本專案目前只產生單一簽署者的 SignerInfo（見
    // signature_creator.cpp），多簽署者的情境不在這條需求的範圍內。
    if (STACK_OF(PKCS7_SIGNER_INFO)* signerInfos = PKCS7_get_signer_info(p7)) {
        if (sk_PKCS7_SIGNER_INFO_num(signerInfos) > 0) {
            PKCS7_SIGNER_INFO* signerInfo = sk_PKCS7_SIGNER_INFO_value(signerInfos, 0);
            report.hasTimestamp = extractSignatureTimestamp(signerInfo, report.timestampGenTimeUnix);
        }
    }

    // 其餘內嵌憑證。順序不保證是鏈的順序，但至少讓面板能列出所有隨附憑證。
    if (p7->d.sign && p7->d.sign->cert) {
        const int count = sk_X509_num(p7->d.sign->cert);
        for (int i = 0; i < count; ++i) {
            X509* cert = sk_X509_value(p7->d.sign->cert, i);
            if (cert == signer) continue;
            report.chain.push_back(describeCertificate(cert));
        }
    }

    if (revocation && signer) {
        // 發證者優先從內嵌憑證找；找不到就退回自己（自簽憑證的情況）。
        X509* issuer = nullptr;
        if (p7->d.sign && p7->d.sign->cert) {
            const int count = sk_X509_num(p7->d.sign->cert);
            for (int i = 0; i < count; ++i) {
                X509* candidate = sk_X509_value(p7->d.sign->cert, i);
                if (X509_NAME_cmp(X509_get_subject_name(candidate),
                                  X509_get_issuer_name(signer)) == 0) {
                    issuer = candidate;
                    break;
                }
            }
        }
        report.revocation = revocation->check(signer, issuer, options.verificationTime);
    }

    PKCS7_free(p7);
    finalize(report, options.revocationPolicy);
    return report;
}

}  // namespace alioth::engine::signature
