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

std::size_t TrustStore::size() const noexcept { return impl_->count; }

void* TrustStore::nativeHandle() const noexcept { return impl_->store; }

}  // namespace alioth::engine::signature
