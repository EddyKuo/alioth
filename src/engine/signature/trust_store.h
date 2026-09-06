#pragma once

// 信任存放區（WBS 6.6 的一部分）。
//
// PRD §4.1 保留 OpenSSL 的理由就是這一段：三平台共用同一份信任判斷，
// 不因作業系統而降級。因此這個類別**不**去讀 Windows 憑證存放區或
// macOS Keychain——那樣做等於把「三平台結果一致」這個賣點退回去。
// 平台原生存放區的匯入屬於平台層的工作，匯入後仍以 DER/PEM 餵進這裡。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alioth::engine::signature {

class TrustStore {
public:
    TrustStore();
    ~TrustStore();

    TrustStore(const TrustStore&) = delete;
    TrustStore& operator=(const TrustStore&) = delete;

    [[nodiscard]] bool addCertificateDer(const std::uint8_t* data, std::size_t size);
    [[nodiscard]] bool addCertificatePem(const std::string& pem);

    // 載入 OpenSSL 的預設憑證路徑。刻意做成明示呼叫而不是建構時自動載入：
    // 預設路徑在不同機器上內容不同，自動載入會讓「三平台結果一致」悄悄失效。
    [[nodiscard]] bool addDefaultPaths();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // 實際型別 X509_STORE*。只給同屬引擎轉接層的驗證器使用。
    [[nodiscard]] void* nativeHandle() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::signature
