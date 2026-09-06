#pragma once

// 文件安全性：密碼與權限（PRD-SEC-002）。
//
// 「設定／移除密碼與權限」在目前的技術堆疊下拆成兩種完全不同的可行性，
// 這裡分開陳述而不是包成一個看似完整、實則半通不通的 API：
//
//   一、讀出加密演算法與權限旗標 —— 可行。/Encrypt 字典的 /V /R /CF 是明文，
//      只有 /O /U 這類字串與串流內容本身被加密；物件層（Alioth::objects）
//      本來就能在不解密的情況下讀 trailer（見 PdfSourceDocument）。
//      權限旗標另外從已解鎖的 FPDF_DOCUMENT 讀出（FPDF_GetDocPermissions）。
//
//   二、移除密碼 —— 可行，而且地基已經在：IncrementalSaver::saveAsCopy 搭配
//      SaveOptions::removeSecurity 就是 FPDF_REMOVE_SECURITY，PDFium 會在整份
//      重寫時原地解密。本檔把它包成語意更明確、帶前置檢查的入口。
//
//   三、設定新密碼 —— 在目前技術堆疊下不可行。PRD 附錄 C 列出的 PDFium 存檔
//      介面（FPDF_SaveWithVersion / FPDF_SaveAsCopy）完全沒有「加密輸出」這個
//      選項；我們自己的物件層寫入通道（ADR-002）也明確拒絕加密文件——它從來
//      不是為了產生加密輸出而設計的。要做到這件事，必須自己實作 ISO 32000-1
//      §7.6 的整條加密管線（RC4/AES 金鑰推導、逐字串逐串流加密），那是一個
//      新的引擎級子系統，不是補一個函式的規模。因此這裡明確回報不支援，
//      不生出一個看起來能用、實際上繞過安全性的假實作。
//      見 exceptions/EXC_20260906_RD_SA_set_password_unsupported.md。

#include <cstdint>
#include <string>

#include "engine/save/incremental_saver.h"

namespace alioth::engine::save {

enum class EncryptionAlgorithm {
    None,     // 未加密
    Rc4_40,   // /V 1（R2）或 /V 2 但金鑰長度 40 位元
    Rc4_128,  // /V 2，金鑰長度 >= 128 位元
    Aes128,   // /V 4，/CF /StdCF /CFM /AESV2
    Aes256,   // /V 5（R5/R6），/CF /StdCF /CFM /AESV3
    Unknown,  // 有 /Encrypt 但無法辨識演算法（自訂安全處理器、公開金鑰安全性等）
};

[[nodiscard]] const char* describe(EncryptionAlgorithm algorithm) noexcept;

// 標準安全處理器的權限旗標（ISO 32000-1 表 22）。位元逐一解碼成具名布林值，
// UI 才能直接顯示「允許／不允許」而不必記位元位置。
struct PermissionFlags {
    bool print{true};
    bool modify{true};
    bool copy{true};
    bool annotate{true};
    bool fillForms{true};
    bool extractForAccessibility{true};
    bool assemble{true};
    bool printHighRes{true};

    // 由 FPDF_GetDocPermissions / FPDF_GetDocUserPermissions 的 32-bit 回傳值解碼。
    // 文件未加密時 PDFium 回傳 0xFFFFFFFF，對應全部允許——即本結構的預設值。
    [[nodiscard]] static PermissionFlags fromRawFlags(std::uint32_t raw) noexcept;
};

struct SecurityInfo {
    bool encrypted{false};
    EncryptionAlgorithm algorithm{EncryptionAlgorithm::None};
    int securityHandlerRevision{-1};  // FPDF_GetSecurityHandlerRevision 的原始值
    PermissionFlags permissions{};
    // /Encrypt 字典是否成功解析出演算法。有 /Encrypt 但這裡是 false 時，
    // UI 應顯示「無法確認演算法」而不是照著預設值亂猜。
    bool algorithmKnown{false};
};

// 只看原始檔案位元組就能回答的部分：是否加密、用哪種演算法、安全處理器
// 修訂版本。不需要密碼、不需要成功解密——/Encrypt 字典本身不是加密內容。
// 權限旗標維持結構預設值（全部允許）；要拿到真正的權限旗標，
// 呼叫端須在文件成功解鎖後另外呼叫 fillPermissions()。
[[nodiscard]] SecurityInfo inspectEncryption(const std::string& pdfBytes);

// 用已解鎖的文件把手補上權限旗標。document 必須是已經用正確密碼
// （或本來就不需要密碼）成功開啟的把手；本函式不重新開檔、不驗證密碼。
void fillPermissions(DocumentHandle document, SecurityInfo& info);

enum class SetPasswordStatus {
    NotSupported,  // 目前技術堆疊下的唯一可能結果，理由見檔頭「三」
};

struct SetPasswordResult {
    SetPasswordStatus status{SetPasswordStatus::NotSupported};
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return false; }  // 恆為 false，不會有第二種結果
};

// 設定或修改密碼。目前唯一行為是回報不支援並解釋原因。
// 保留這個函式簽章（而不是完全不提供 API）是刻意的：呼叫端／UI 可以直接接上
// 「設定密碼」選單項，收到明確的不支援訊息，而不是選單項目根本不存在、
// 讓人誤以為這個功能還沒排進開發計畫（IL-4：失敗必須明確回報）。
[[nodiscard]] SetPasswordResult setPassword(const std::string& newUserPassword,
                                            const std::string& newOwnerPassword,
                                            std::uint32_t permissions);

// 移除密碼與加密：對已解鎖的文件把手整份重寫（FPDF_REMOVE_SECURITY）。
// 這條路徑會破壞既有數位簽章（整份重寫，物件編號全部重排）——呼叫端必須先
// 確認文件沒有簽章，或已比照 optimize_saver 的作法告知使用者這個後果。
[[nodiscard]] SaveResult removePassword(DocumentHandle document, const std::string& targetPath);

}  // namespace alioth::engine::save
