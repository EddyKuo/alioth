#include "engine/signature/revocation.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>

namespace alioth::engine::signature {
namespace {

std::time_t effectiveTime(std::int64_t at) {
    return at > 0 ? static_cast<std::time_t>(at) : std::time(nullptr);
}

}  // namespace

struct CrlRevocationChecker::Impl {
    std::vector<X509_CRL*> crls;

    ~Impl() {
        for (X509_CRL* crl : crls) X509_CRL_free(crl);
    }
};

CrlRevocationChecker::CrlRevocationChecker() : impl_(std::make_unique<Impl>()) {}
CrlRevocationChecker::~CrlRevocationChecker() = default;

bool CrlRevocationChecker::addCrlDer(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) return false;
    const unsigned char* cursor = data;
    X509_CRL* crl = d2i_X509_CRL(nullptr, &cursor, static_cast<long>(size));
    if (!crl) {
        ERR_clear_error();
        return false;
    }
    impl_->crls.push_back(crl);
    return true;
}

bool CrlRevocationChecker::addCrlPem(const std::string& pem) {
    if (pem.empty()) return false;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return false;
    std::size_t added = 0;
    while (X509_CRL* crl = PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr)) {
        impl_->crls.push_back(crl);
        ++added;
    }
    ERR_clear_error();
    BIO_free(bio);
    return added > 0;
}

std::size_t CrlRevocationChecker::crlCount() const noexcept { return impl_->crls.size(); }

RevocationStatus CrlRevocationChecker::check(void* subject, void* issuer, std::int64_t at) {
    auto* cert = static_cast<X509*>(subject);
    auto* issuerCert = static_cast<X509*>(issuer);
    if (!cert) return RevocationStatus::Unknown;

    std::time_t now = effectiveTime(at);
    bool sawApplicableCrl = false;

    for (X509_CRL* crl : impl_->crls) {
        // CRL 必須由該憑證的發證者簽發，否則任何人都能自己簽一份
        // 「這張憑證沒被吊銷」的 CRL 來蓋掉真正的吊銷紀錄。
        if (X509_NAME_cmp(X509_CRL_get_issuer(crl), X509_get_issuer_name(cert)) != 0) continue;

        if (issuerCert) {
            EVP_PKEY* key = X509_get0_pubkey(issuerCert);
            if (!key || X509_CRL_verify(crl, key) != 1) {
                ERR_clear_error();
                continue;
            }
        }

        // 過期的 CRL 不能拿來證明「未被吊銷」——它只證明簽發當下未被吊銷。
        const ASN1_TIME* nextUpdate = X509_CRL_get0_nextUpdate(crl);
        if (nextUpdate && X509_cmp_time(nextUpdate, &now) < 0) continue;

        sawApplicableCrl = true;
        X509_REVOKED* revoked = nullptr;
        if (X509_CRL_get0_by_cert(crl, &revoked, cert) == 1) {
            return RevocationStatus::Revoked;
        }
    }

    return sawApplicableCrl ? RevocationStatus::Good : RevocationStatus::Unknown;
}

struct OcspRevocationChecker::Impl {
    Transport transport;
};

OcspRevocationChecker::OcspRevocationChecker(Transport transport)
    : impl_(std::make_unique<Impl>()) {
    impl_->transport = std::move(transport);
}

OcspRevocationChecker::~OcspRevocationChecker() = default;

std::string OcspRevocationChecker::responderUrl(void* subject) {
    auto* cert = static_cast<X509*>(subject);
    if (!cert) return {};
    std::string url;
    STACK_OF(OPENSSL_STRING)* urls = X509_get1_ocsp(cert);
    if (urls) {
        if (sk_OPENSSL_STRING_num(urls) > 0) {
            const char* first = sk_OPENSSL_STRING_value(urls, 0);
            if (first) url = first;
        }
        X509_email_free(urls);
    }
    ERR_clear_error();
    return url;
}

RevocationStatus OcspRevocationChecker::check(void* subject, void* issuer, std::int64_t at) {
    auto* cert = static_cast<X509*>(subject);
    auto* issuerCert = static_cast<X509*>(issuer);
    // 沒有發證者憑證就組不出 CertID；沒有傳輸就送不出去。
    // 兩種情況都回 Unknown——「沒查成功」不等於「沒問題」。
    if (!cert || !issuerCert || !impl_->transport) return RevocationStatus::Unknown;

    const std::string url = responderUrl(cert);
    if (url.empty()) return RevocationStatus::Unknown;

    OCSP_REQUEST* request = OCSP_REQUEST_new();
    if (!request) return RevocationStatus::Unknown;

    OCSP_CERTID* id = OCSP_cert_to_id(nullptr, cert, issuerCert);
    if (!id || !OCSP_request_add0_id(request, id)) {
        if (id) OCSP_CERTID_free(id);
        OCSP_REQUEST_free(request);
        ERR_clear_error();
        return RevocationStatus::Unknown;
    }

    unsigned char* der = nullptr;
    const int derLength = i2d_OCSP_REQUEST(request, &der);
    std::vector<std::uint8_t> encoded;
    if (derLength > 0 && der) {
        encoded.assign(der, der + derLength);
    }
    if (der) OPENSSL_free(der);
    OCSP_REQUEST_free(request);
    if (encoded.empty()) {
        ERR_clear_error();
        return RevocationStatus::Unknown;
    }

    const std::vector<std::uint8_t> responseBytes = impl_->transport(url, encoded);
    if (responseBytes.empty()) return RevocationStatus::Unknown;

    const unsigned char* cursor = responseBytes.data();
    OCSP_RESPONSE* response =
        d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(responseBytes.size()));
    if (!response) {
        ERR_clear_error();
        return RevocationStatus::Unknown;
    }

    RevocationStatus status = RevocationStatus::Unknown;
    if (OCSP_response_status(response) == OCSP_RESPONSE_STATUS_SUCCESSFUL) {
        OCSP_BASICRESP* basic = OCSP_response_get1_basic(response);
        if (basic) {
            OCSP_CERTID* queryId = OCSP_cert_to_id(nullptr, cert, issuerCert);
            int certStatus = 0;
            int reason = 0;
            ASN1_GENERALIZEDTIME* revokedAt = nullptr;
            ASN1_GENERALIZEDTIME* thisUpdate = nullptr;
            ASN1_GENERALIZEDTIME* nextUpdate = nullptr;
            if (queryId && OCSP_resp_find_status(basic, queryId, &certStatus, &reason, &revokedAt,
                                                 &thisUpdate, &nextUpdate)) {
                // 刻意不在這裡驗證回應簽章：那需要 responder 的信任錨，
                // 而信任錨屬於 TrustStore。目前的取捨是回應簽章未驗證，
                // 因此本路徑只在有可信傳輸的環境下才應啟用（見標頭說明）。
                if (certStatus == V_OCSP_CERTSTATUS_GOOD) {
                    status = RevocationStatus::Good;
                } else if (certStatus == V_OCSP_CERTSTATUS_REVOKED) {
                    status = RevocationStatus::Revoked;
                }
            }
            if (queryId) OCSP_CERTID_free(queryId);
            OCSP_BASICRESP_free(basic);
        }
    }
    OCSP_RESPONSE_free(response);
    ERR_clear_error();
    (void)at;
    return status;
}

}  // namespace alioth::engine::signature
