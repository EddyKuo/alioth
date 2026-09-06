#pragma once

// 簽署身分：私鑰與簽署憑證（WBS 6.11 的一部分，PRD-SIG-004）。
//
// 與 TrustStore 對稱：驗證側（trust_store.h）持有「誰是可信任的根」，這裡持有
// 「誰要簽」。私鑰全程只活在記憶體，不寫暫存檔、不記錄到任何日誌；載入失敗時
// 不留下任何部分狀態（要嘛完全可用，要嘛 valid() 為 false）。
//
// 只支援 PKCS#12（.pfx/.p12，使用者實務上最常見的憑證交付格式）與 PEM
// （測試、以及已拆分金鑰/憑證的情境）。不支援讀取作業系統憑證存放區
// （Windows CNG／macOS Keychain）——理由與 TrustStore 相同：那樣做會讓
// 「三平台簽章行為一致」這個賣點在建立簽章這一側也開始鬆動。平台原生存放區的
// 匯出屬於平台層的工作，匯出後仍以 PKCS#12 或 PEM 餵進這裡。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace alioth::engine::signature {

class SigningIdentity {
public:
    SigningIdentity();
    ~SigningIdentity();

    SigningIdentity(const SigningIdentity&) = delete;
    SigningIdentity& operator=(const SigningIdentity&) = delete;

    // 解析失敗（密碼錯誤、格式損毀、容器內沒有私鑰）一律回傳 false 且不留下
    // 部分載入的憑證或金鑰——valid() 仍然是唯一該檢查的旗標。
    [[nodiscard]] bool loadPkcs12(const std::uint8_t* data, std::size_t size,
                                  const std::string& password);
    [[nodiscard]] bool loadPem(const std::string& certPem, const std::string& keyPem,
                               const std::string& keyPassword = {});

    // 中介憑證鏈（不含簽署憑證本身），會一併放進 CMS 的憑證集合，讓驗證端
    // 不必另外提供中介憑證就能組出完整鏈。PKCS#12 容器裡的中介憑證會在
    // loadPkcs12 時自動收進來，這兩個函式是給 PEM 路徑或想額外補件的情境用的。
    [[nodiscard]] bool addChainCertificatePem(const std::string& pem);
    [[nodiscard]] bool addChainCertificateDer(const std::uint8_t* data, std::size_t size);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string subjectCommonName() const;

    // 實際型別 X509* / EVP_PKEY* / STACK_OF(X509)*。只給同屬引擎轉接層的
    // 簽章建立器（signature_creator.h）使用，OpenSSL 標頭不外洩到這個模組以外。
    [[nodiscard]] void* nativeCert() const noexcept;
    [[nodiscard]] void* nativeKey() const noexcept;
    [[nodiscard]] void* nativeChain() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::signature
