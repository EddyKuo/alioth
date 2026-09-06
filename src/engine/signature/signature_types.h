#pragma once

// 簽章驗證的三態結果模型（WBS 6.7，PRD-SIG-001）。
//
// PRD 要求綠／黃／紅三態，而且「不受信任根憑證必須明確標示，不得混進有效」。
// 那句話的實作意義是：Trust 這個列舉只有三個值，而且**沒有任何路徑**能讓
// 未通過憑證鏈驗證的簽章得到 Trusted。分類規則集中在 classify() 一個函式裡，
// 讓它可以被單獨測試，也讓「什麼情況算綠燈」永遠只有一份定義（IL-3）。

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "engine/signature/byte_range.h"

namespace alioth::engine::signature {

enum class SignatureTrust {
    Invalid,    // 紅：密碼學驗證失敗、檔案被竄改、或簽章未涵蓋全檔
    Untrusted,  // 黃：密碼學上有效，但憑證鏈不受信任或吊銷狀態無法確認
    Trusted,    // 綠：有效且憑證鏈可回溯到受信任的根憑證
};

[[nodiscard]] const char* describe(SignatureTrust trust) noexcept;

enum class RevocationStatus {
    NotChecked,  // 未查詢（離線、或使用者關閉查詢）
    Good,
    Revoked,
    Unknown,     // 查詢過但拿不到明確答案
};

[[nodiscard]] const char* describe(RevocationStatus status) noexcept;

// 吊銷查詢政策。預設 SoftFail 而不是 Skip：
// 「查不到吊銷狀態」在 PRD 的三態定義裡屬於黃燈，靜默當成綠燈是說謊。
enum class RevocationPolicy {
    Skip,      // 明確不查（使用者選擇離線驗證）。不因未查而降級
    SoftFail,  // 查不到就降為黃燈
    HardFail,  // 查不到就視為無效（高合規情境）
};

struct CertificateInfo {
    std::string subject;
    std::string issuer;
    std::string serialHex;
    std::string sha256Fingerprint;
    std::int64_t notBefore{0};  // Unix 時間
    std::int64_t notAfter{0};
    bool selfSigned{false};
};

struct VerifyOptions {
    RevocationPolicy revocationPolicy{RevocationPolicy::SoftFail};
    // 驗證基準時間。0 代表用系統當下時間。
    // 開放這個參數是為了讓測試能固定時間，而不是靠等憑證過期。
    std::int64_t verificationTime{0};
};

struct SignatureReport {
    std::int32_t index{0};
    std::string subFilter;       // adbe.pkcs7.detached / ETSI.CAdES.detached ...
    std::string signerName;      // 憑證主體的 CN
    std::string reason;          // /Reason
    std::string signingTimeRaw;  // /M，格式 D:YYYYMMDDHHMMSS+XX'YY'

    ByteRangeCheck coverage;

    bool contentsPresent{false};
    bool parsed{false};                  // PKCS#7 / CMS 結構解析成功
    bool digestMatches{false};           // 內含摘要與實際位元組相符
    bool cryptographicallyValid{false};  // 簽章數學驗證通過
    bool chainTrusted{false};            // 憑證鏈可回溯到信任存放區
    bool certificateExpired{false};
    bool certificateNotYetValid{false};
    RevocationStatus revocation{RevocationStatus::NotChecked};

    // PRD-SIG-006（PAdES-B-T）。見 pkcs7_verifier.h 開頭對這兩個欄位範圍的
    // 保守說明：目前只是「看得見」，尚未接進 trust 判色邏輯。
    bool hasTimestamp{false};
    std::int64_t timestampGenTimeUnix{0};

    std::vector<CertificateInfo> chain;  // [0] 是簽署者憑證
    std::vector<std::string> findings;   // 繁體中文，逐條說明為什麼是這個顏色

    SignatureTrust trust{SignatureTrust::Invalid};
};

// 三態分類。這是唯一決定燈號的地方。
[[nodiscard]] SignatureTrust classify(const SignatureReport& report, RevocationPolicy policy);

// 依報告內容補上 findings 與 trust。verify 流程最後一步呼叫，
// 也可對手工組出的報告呼叫——這讓分類規則能脫離 OpenSSL 單獨測試。
void finalize(SignatureReport& report, RevocationPolicy policy);

}  // namespace alioth::engine::signature
