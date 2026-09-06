#pragma once

// 簽章測試語料產生器。
//
// 帶簽章的 PDF 很難手造，但也不能不造：沒有可控的正例，就無法證明
// 「改一個位元組必須失敗」這種負例真的是因為竄改而失敗，而不是因為
// 我們的解析從頭到尾都是錯的。
//
// 做法是自己走完真正的簽章流程：
//   1. 用 OpenSSL 產生自簽 CA 與一張由它簽發的簽署憑證
//   2. 組出帶 /ByteRange 與 /Contents 佔位符的 PDF
//   3. 依 /Contents 的實際位置回填 /ByteRange（必須在算摘要之前）
//   4. 對 /ByteRange 涵蓋的位元組做 detached PKCS#7 簽章
//   5. 把 DER 轉十六進位填回 /Contents，尾端補零到佔位長度
//
// 有了這份正例，負例就是它的衍生：改一個位元組、在尾端附加未涵蓋的內容、
// 用過期憑證重簽、用列出該序號的 CRL 查詢。

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <QByteArray>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace alioth::test {

// 一組憑證與私鑰。刻意不做成可複製：X509 與 EVP_PKEY 是引用計數物件，
// 隨手複製會讓釋放時機難以推理。
struct Identity {
    EVP_PKEY* key{nullptr};
    X509* cert{nullptr};

    Identity() = default;
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;
    Identity(Identity&& other) noexcept : key(other.key), cert(other.cert) {
        other.key = nullptr;
        other.cert = nullptr;
    }
    Identity& operator=(Identity&& other) noexcept {
        if (this != &other) {
            if (cert) X509_free(cert);
            if (key) EVP_PKEY_free(key);
            key = other.key;
            cert = other.cert;
            other.key = nullptr;
            other.cert = nullptr;
        }
        return *this;
    }
    ~Identity() {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
    }

    [[nodiscard]] bool valid() const { return key != nullptr && cert != nullptr; }

    [[nodiscard]] std::vector<std::uint8_t> certDer() const {
        unsigned char* buffer = nullptr;
        const int length = i2d_X509(cert, &buffer);
        std::vector<std::uint8_t> out;
        if (length > 0 && buffer) out.assign(buffer, buffer + length);
        if (buffer) OPENSSL_free(buffer);
        return out;
    }
};

inline void setExtension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) {
        ERR_clear_error();
        return;
    }
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

// notBefore / notAfter 以「相對於現在的秒數」表示，讓過期憑證可以直接造出來，
// 而不必等時間過去。
inline Identity makeIdentity(const std::string& commonName, const Identity* issuer,
                             long notBeforeSeconds, long notAfterSeconds, long serial,
                             bool isCa) {
    Identity identity;
    identity.key = EVP_RSA_gen(2048);
    if (!identity.key) return identity;

    identity.cert = X509_new();
    if (!identity.cert) return identity;

    X509_set_version(identity.cert, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(identity.cert), serial);
    X509_gmtime_adj(X509_getm_notBefore(identity.cert), notBeforeSeconds);
    X509_gmtime_adj(X509_getm_notAfter(identity.cert), notAfterSeconds);
    X509_set_pubkey(identity.cert, identity.key);

    X509_NAME* subject = X509_get_subject_name(identity.cert);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1,
                               0);
    X509_NAME_add_entry_by_txt(subject, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("Alioth Test"), -1, -1, 0);

    X509* issuerCert = issuer ? issuer->cert : identity.cert;
    EVP_PKEY* issuerKey = issuer ? issuer->key : identity.key;
    X509_set_issuer_name(identity.cert, X509_get_subject_name(issuerCert));

    if (isCa) {
        setExtension(identity.cert, issuerCert, NID_basic_constraints, "critical,CA:TRUE");
        setExtension(identity.cert, issuerCert, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        setExtension(identity.cert, issuerCert, NID_basic_constraints, "critical,CA:FALSE");
        setExtension(identity.cert, issuerCert, NID_key_usage,
                     "critical,digitalSignature,nonRepudiation");
        // 沒有 emailProtection 的話 PKCS7_verify 的預設用途檢查（S/MIME 簽章）會擋下來，
        // 症狀會被誤讀成「憑證鏈不受信任」。
        setExtension(identity.cert, issuerCert, NID_ext_key_usage, "emailProtection");
    }
    setExtension(identity.cert, issuerCert, NID_subject_key_identifier, "hash");

    if (X509_sign(identity.cert, issuerKey, EVP_sha256()) == 0) {
        ERR_clear_error();
        X509_free(identity.cert);
        identity.cert = nullptr;
    }
    return identity;
}

inline std::string certificatePem(const Identity& identity) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return {};
    PEM_write_bio_X509(bio, identity.cert);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string out;
    if (length > 0 && data) out.assign(data, static_cast<std::size_t>(length));
    BIO_free(bio);
    return out;
}

// Detached PKCS#7。extraCerts 會一併放進 SignedData 的憑證集合，
// 讓驗證端不必另外提供中介憑證。
inline std::vector<std::uint8_t> signDetached(const Identity& signer,
                                              const std::vector<std::uint8_t>& data,
                                              const Identity* extraCert) {
    std::vector<std::uint8_t> out;
    BIO* bio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
    if (!bio) return out;

    STACK_OF(X509)* chain = sk_X509_new_null();
    if (extraCert && chain) sk_X509_push(chain, extraCert->cert);

    PKCS7* p7 = PKCS7_sign(signer.cert, signer.key, chain, bio,
                           PKCS7_DETACHED | PKCS7_BINARY);
    if (p7) {
        unsigned char* buffer = nullptr;
        const int length = i2d_PKCS7(p7, &buffer);
        if (length > 0 && buffer) out.assign(buffer, buffer + length);
        if (buffer) OPENSSL_free(buffer);
        PKCS7_free(p7);
    } else {
        ERR_clear_error();
    }
    if (chain) sk_X509_free(chain);
    BIO_free(bio);
    return out;
}

// 由 issuer 簽發的 CRL。revokedSerials 為空即「這張憑證沒被吊銷」的證據。
inline std::vector<std::uint8_t> makeCrl(const Identity& issuer,
                                         const std::vector<long>& revokedSerials,
                                         long nextUpdateSeconds) {
    std::vector<std::uint8_t> out;
    X509_CRL* crl = X509_CRL_new();
    if (!crl) return out;

    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(issuer.cert));

    ASN1_TIME* lastUpdate = ASN1_TIME_adj(nullptr, time(nullptr), 0, -3600);
    ASN1_TIME* nextUpdate = ASN1_TIME_adj(nullptr, time(nullptr), 0, nextUpdateSeconds);
    X509_CRL_set1_lastUpdate(crl, lastUpdate);
    X509_CRL_set1_nextUpdate(crl, nextUpdate);
    ASN1_TIME_free(lastUpdate);
    ASN1_TIME_free(nextUpdate);

    for (const long serial : revokedSerials) {
        X509_REVOKED* revoked = X509_REVOKED_new();
        ASN1_INTEGER* number = ASN1_INTEGER_new();
        ASN1_INTEGER_set(number, serial);
        X509_REVOKED_set_serialNumber(revoked, number);
        ASN1_INTEGER_free(number);
        ASN1_TIME* when = ASN1_TIME_adj(nullptr, time(nullptr), 0, -1800);
        X509_REVOKED_set_revocationDate(revoked, when);
        ASN1_TIME_free(when);
        X509_CRL_add0_revoked(crl, revoked);
    }
    X509_CRL_sort(crl);

    if (X509_CRL_sign(crl, issuer.key, EVP_sha256()) != 0) {
        unsigned char* buffer = nullptr;
        const int length = i2d_X509_CRL(crl, &buffer);
        if (length > 0 && buffer) out.assign(buffer, buffer + length);
        if (buffer) OPENSSL_free(buffer);
    } else {
        ERR_clear_error();
    }
    X509_CRL_free(crl);
    return out;
}

// 預留給 /Contents 的位元組數。RSA-2048 兩張憑證的 PKCS#7 約 2 KB，
// 留 6 KB 是常見做法：太小會在簽章當下才發現放不下，那時已經來不及改。
inline constexpr int kSignatureReserveBytes = 6000;

inline QByteArray padded(int value) {
    return QByteArray::number(value).rightJustified(10, '0');
}

// 帶簽章欄位但 /ByteRange 與 /Contents 仍是佔位符的 PDF。
inline QByteArray makeUnsignedSignaturePdfTemplate() {
    const QByteArray content = "0 0 0 rg\n20 20 160 100 re\nf\n";
    const QByteArray contentsPlaceholder(kSignatureReserveBytes * 2, '0');

    std::vector<QByteArray> objects;
    objects.push_back(
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] /SigFlags 3 >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents 4 0 R "
        "/Resources << >> /Annots [5 0 R] >>");
    objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                      content + "\nendstream");
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1) /V 6 0 R /F 132 "
        "/Rect [0 0 0 0] /P 3 0 R >>");
    objects.push_back(
        "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached "
        "/M (D:20260101120000+08'00') /Reason (Alioth test) "
        "/ByteRange [0 " + padded(0) + " " + padded(0) + " " + padded(0) + "] "
        "/Contents <" + contentsPlaceholder + "> >>");

    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

struct SignedPdf {
    QByteArray bytes;
    int contentsStart{0};  // '<' 的位置
    int contentsEnd{0};    // '>' 之後的位置
    bool ok{false};
};

// 對範本完成簽章。coverTail 為假時刻意讓最後一段長度短少 tailShortfall 位元組，
// 用來造出「/ByteRange 不涵蓋全檔」的偽造情境。
inline SignedPdf signPdfTemplate(const Identity& signer, const Identity* chainCert,
                                 QByteArray pdf) {
    SignedPdf result;

    const int contentsMarker = pdf.indexOf("/Contents <");
    if (contentsMarker < 0) return result;
    const int start = pdf.indexOf('<', contentsMarker);
    const int end = pdf.indexOf('>', start);
    if (start < 0 || end < 0) return result;

    result.contentsStart = start;
    result.contentsEnd = end + 1;

    const int tailStart = result.contentsEnd;
    const int tailLength = pdf.size() - tailStart;

    // /ByteRange 必須在算摘要之前寫定：它自己也在被簽的區段裡，
    // 先簽再改會讓簽章立刻失效——這是自製簽章最常見的錯誤。
    const int rangeMarker = pdf.indexOf("/ByteRange [0 ");
    if (rangeMarker < 0) return result;
    const int digitsStart = rangeMarker + static_cast<int>(std::strlen("/ByteRange [0 "));
    const QByteArray rangeText =
        padded(result.contentsStart) + " " + padded(tailStart) + " " + padded(tailLength);
    pdf.replace(digitsStart, rangeText.size(), rangeText);

    std::vector<std::uint8_t> signedBytes;
    signedBytes.reserve(static_cast<std::size_t>(result.contentsStart + tailLength));
    signedBytes.insert(signedBytes.end(),
                       reinterpret_cast<const std::uint8_t*>(pdf.constData()),
                       reinterpret_cast<const std::uint8_t*>(pdf.constData()) +
                           result.contentsStart);
    signedBytes.insert(signedBytes.end(),
                       reinterpret_cast<const std::uint8_t*>(pdf.constData()) + tailStart,
                       reinterpret_cast<const std::uint8_t*>(pdf.constData()) + pdf.size());

    const std::vector<std::uint8_t> der = signDetached(signer, signedBytes, chainCert);
    if (der.empty() || static_cast<int>(der.size()) > kSignatureReserveBytes) return result;

    QByteArray hex;
    hex.reserve(kSignatureReserveBytes * 2);
    static const char* kHex = "0123456789ABCDEF";
    for (const std::uint8_t byte : der) {
        hex.append(kHex[byte >> 4]);
        hex.append(kHex[byte & 0x0F]);
    }
    hex.append(QByteArray(kSignatureReserveBytes * 2 - hex.size(), '0'));

    pdf.replace(result.contentsStart + 1, hex.size(), hex);
    result.bytes = pdf;
    result.ok = true;
    return result;
}

inline SignedPdf makeSignedPdf(const Identity& signer, const Identity* chainCert) {
    return signPdfTemplate(signer, chainCert, makeUnsignedSignaturePdfTemplate());
}

// 在簽章之後於檔尾附加未被 /ByteRange 涵蓋的內容。
//
// 這正是 Incremental Saving Attack：附加的位元組不影響密碼學驗證，
// 但它們會被閱讀器顯示出來。閱讀器若只回報「簽章有效」就等於替偽造背書。
inline QByteArray appendUncoveredContent(const QByteArray& signedPdf) {
    const int startxref = signedPdf.lastIndexOf("startxref");
    if (startxref < 0) return signedPdf;
    QByteArray tail = signedPdf.mid(startxref);
    return signedPdf + "\n% injected content that no signature covers\n" + tail;
}

}  // namespace alioth::test
