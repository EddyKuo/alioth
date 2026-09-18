#include "engine/signature/trust_store.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

namespace alioth::engine::signature {

struct TrustStore::Impl {
    X509_STORE* store{nullptr};
    std::size_t count{0};
};

TrustStore::TrustStore() : impl_(std::make_unique<Impl>()) {
    impl_->store = X509_STORE_new();
}

TrustStore::~TrustStore() {
    if (impl_->store) X509_STORE_free(impl_->store);
}

bool TrustStore::addCertificateDer(const std::uint8_t* data, std::size_t size) {
    if (!impl_->store || !data || size == 0) return false;
    const unsigned char* cursor = data;
    X509* cert = d2i_X509(nullptr, &cursor, static_cast<long>(size));
    if (!cert) {
        // 解析失敗會在錯誤佇列留下紀錄。不清掉的話，後面某個無關的
        // ERR_get_error() 會撿到這一筆，於是錯誤訊息指向完全錯誤的地方。
        ERR_clear_error();
        return false;
    }
    const bool ok = X509_STORE_add_cert(impl_->store, cert) == 1;
    X509_free(cert);
    if (ok) ++impl_->count;
    else ERR_clear_error();
    return ok;
}

bool TrustStore::addCertificatePem(const std::string& pem) {
    if (!impl_->store || pem.empty()) return false;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return false;

    std::size_t added = 0;
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        if (X509_STORE_add_cert(impl_->store, cert) == 1) ++added;
        X509_free(cert);
    }
    ERR_clear_error();
    BIO_free(bio);
    impl_->count += added;
    return added > 0;
}

bool TrustStore::addDefaultPaths() {
    if (!impl_->store) return false;
    const bool ok = X509_STORE_set_default_paths(impl_->store) == 1;
    if (!ok) ERR_clear_error();
    return ok;
}

namespace {

// X509_NAME 轉可讀字串。oneline 的輸出是 "/C=TW/O=.../CN=..."，
// 對「這是不是我以為的那一張」這個問題已經夠用，而且不必自己走 RDN。
std::string nameToString(X509_NAME* name) {
    if (name == nullptr) return {};
    char buffer[512] = {};
    if (X509_NAME_oneline(name, buffer, static_cast<int>(sizeof(buffer))) == nullptr) return {};
    return std::string(buffer);
}

std::string timeToString(const ASN1_TIME* time) {
    if (time == nullptr) return {};
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) return {};
    std::string out;
    if (ASN1_TIME_print(bio, time) == 1) {
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio, &data);
        if (data != nullptr && length > 0) out.assign(data, static_cast<std::size_t>(length));
    }
    BIO_free(bio);
    ERR_clear_error();
    return out;
}

}  // namespace

std::vector<TrustedCertificate> TrustStore::certificates() const {
    std::vector<TrustedCertificate> out;
    if (impl_->store == nullptr) return out;

    STACK_OF(X509_OBJECT)* objects = X509_STORE_get0_objects(impl_->store);
    if (objects == nullptr) return out;

    const int count = sk_X509_OBJECT_num(objects);
    out.reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    for (int i = 0; i < count; ++i) {
        X509_OBJECT* entry = sk_X509_OBJECT_value(objects, i);
        if (entry == nullptr) continue;
        X509* cert = X509_OBJECT_get0_X509(entry);
        if (cert == nullptr) continue;  // CRL 之類的項目也在同一個堆疊裡

        TrustedCertificate summary;
        summary.subject = nameToString(X509_get_subject_name(cert));
        summary.issuer = nameToString(X509_get_issuer_name(cert));
        summary.notAfter = timeToString(X509_get0_notAfter(cert));
        out.push_back(std::move(summary));
    }
    return out;
}

std::size_t TrustStore::size() const noexcept { return impl_->count; }

void* TrustStore::nativeHandle() const noexcept { return impl_->store; }

}  // namespace alioth::engine::signature
