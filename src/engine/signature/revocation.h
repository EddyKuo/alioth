#pragma once

// 憑證吊銷查詢（WBS 6.6 的一部分）。
//
// 分工刻意切在「協定」與「傳輸」之間：
//   - 協定（CRL 解析、OCSP 請求組裝與回應解析）在引擎轉接層，三平台共用
//   - 傳輸（HTTP）不在這裡。網路存取屬於平台層，而且「驗證一份 PDF 會不會
//     自動連外」是使用者必須能控制的隱私決策，不該由引擎自作主張
//
// 因此 OcspRevocationChecker 需要呼叫端注入 Transport。沒注入時回傳 Unknown
// 而不是 Good——把「沒查」講成「沒問題」正是三態模型要防的事。
//
// R1 的預設路徑是離線驗證：CrlRevocationChecker 由呼叫端餵入 CRL 位元組，
// 完全不連網也能得到明確的 Good / Revoked。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/signature/signature_types.h"

namespace alioth::engine::signature {

// subject / issuer 的實際型別是 X509*。介面以 void* 表達，讓 OpenSSL 標頭
// 不外洩到引擎轉接層以外。
class RevocationChecker {
public:
    virtual ~RevocationChecker() = default;

    // at 是驗證基準時間（Unix 秒）。0 代表使用系統當下時間。
    [[nodiscard]] virtual RevocationStatus check(void* subject, void* issuer,
                                                 std::int64_t at) = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

// 離線 CRL 查詢。CRL 由呼叫端提供（隨文件附帶、或事先下載）。
class CrlRevocationChecker final : public RevocationChecker {
public:
    CrlRevocationChecker();
    ~CrlRevocationChecker() override;

    [[nodiscard]] bool addCrlDer(const std::uint8_t* data, std::size_t size);
    [[nodiscard]] bool addCrlPem(const std::string& pem);
    [[nodiscard]] std::size_t crlCount() const noexcept;

    [[nodiscard]] RevocationStatus check(void* subject, void* issuer, std::int64_t at) override;
    [[nodiscard]] const char* name() const noexcept override { return "CRL"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// OCSP 查詢。協定在這裡，傳輸由外面注入。
class OcspRevocationChecker final : public RevocationChecker {
public:
    // 回傳空向量代表傳輸失敗；本類別會據此回報 Unknown 而不是 Good。
    using Transport = std::function<std::vector<std::uint8_t>(
        const std::string& url, const std::vector<std::uint8_t>& derRequest)>;

    explicit OcspRevocationChecker(Transport transport = {});
    ~OcspRevocationChecker() override;

    [[nodiscard]] RevocationStatus check(void* subject, void* issuer, std::int64_t at) override;
    [[nodiscard]] const char* name() const noexcept override { return "OCSP"; }

    // 從憑證的 AIA 擴充取出 OCSP 責任者 URL。無 AIA 時回傳空字串。
    [[nodiscard]] static std::string responderUrl(void* subject);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::signature
