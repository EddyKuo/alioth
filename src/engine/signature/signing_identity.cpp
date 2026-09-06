#include "engine/signature/signing_identity.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace alioth::engine::signature {

struct SigningIdentity::Impl {
    X509* cert{nullptr};
    EVP_PKEY* key{nullptr};
    STACK_OF(X509)* chain{nullptr};

    void reset() {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
        if (chain) sk_X509_pop_free(chain, X509_free);
        cert = nullptr;
        key = nullptr;
        chain = nullptr;
    }

    ~Impl() { reset(); }
};

SigningIdentity::SigningIdentity() : impl_(std::make_unique<Impl>()) {}
SigningIdentity::~SigningIdentity() = default;

bool SigningIdentity::loadPkcs12(const std::uint8_t* data, std::size_t size,
                                 const std::string& password) {
    impl_->reset();
    if (!data || size == 0) return false;

    BIO* bio = BIO_new_mem_buf(data, static_cast<int>(size));
    if (!bio) return false;
    PKCS12* p12 = d2i_PKCS12_bio(bio, nullptr);
    BIO_free(bio);
    if (!p12) {
        ERR_clear_error();
        return false;
    }

    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* extraCerts = nullptr;
    const int parsed = PKCS12_parse(p12, password.c_str(), &key, &cert, &extraCerts);
    PKCS12_free(p12);
    if (parsed != 1 || !key || !cert) {
        ERR_clear_error();
        if (key) EVP_PKEY_free(key);
        if (cert) X509_free(cert);
        if (extraCerts) sk_X509_pop_free(extraCerts, X509_free);
        return false;
    }

    impl_->key = key;
    impl_->cert = cert;
    impl_->chain = extraCerts;  // 可能是 nullptr（沒有中介憑證），呼叫端的取用皆已容忍 nullptr
    return true;
}

bool SigningIdentity::loadPem(const std::string& certPem, const std::string& keyPem,
                              const std::string& keyPassword) {
    impl_->reset();
    if (certPem.empty() || keyPem.empty()) return false;

    BIO* certBio = BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size()));
    X509* cert = certBio ? PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr) : nullptr;
    if (certBio) BIO_free(certBio);
    if (!cert) {
        ERR_clear_error();
        return false;
    }

    BIO* keyBio = BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size()));
    void* passArg = const_cast<char*>(keyPassword.empty() ? nullptr : keyPassword.c_str());
    EVP_PKEY* key = keyBio ? PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, passArg) : nullptr;
    if (keyBio) BIO_free(keyBio);
    if (!key) {
        ERR_clear_error();
        X509_free(cert);
        return false;
    }

    impl_->cert = cert;
    impl_->key = key;
    return true;
}

bool SigningIdentity::addChainCertificatePem(const std::string& pem) {
    if (pem.empty()) return false;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return false;
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) {
        ERR_clear_error();
        return false;
    }
    if (!impl_->chain) impl_->chain = sk_X509_new_null();
    if (!impl_->chain || sk_X509_push(impl_->chain, cert) <= 0) {
        X509_free(cert);
        return false;
    }
    return true;
}

bool SigningIdentity::addChainCertificateDer(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) return false;
    const unsigned char* cursor = data;
    X509* cert = d2i_X509(nullptr, &cursor, static_cast<long>(size));
    if (!cert) {
        ERR_clear_error();
        return false;
    }
    if (!impl_->chain) impl_->chain = sk_X509_new_null();
    if (!impl_->chain || sk_X509_push(impl_->chain, cert) <= 0) {
        X509_free(cert);
        return false;
    }
    return true;
}

bool SigningIdentity::valid() const noexcept { return impl_->cert != nullptr && impl_->key != nullptr; }

std::string SigningIdentity::subjectCommonName() const {
    if (!impl_->cert) return {};
    X509_NAME* name = X509_get_subject_name(impl_->cert);
    if (!name) return {};
    const int index = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
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

void* SigningIdentity::nativeCert() const noexcept { return impl_->cert; }
void* SigningIdentity::nativeKey() const noexcept { return impl_->key; }
void* SigningIdentity::nativeChain() const noexcept { return impl_->chain; }

}  // namespace alioth::engine::signature
