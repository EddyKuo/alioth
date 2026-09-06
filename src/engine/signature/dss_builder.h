#pragma once

// 長期驗證的證據存放區（WBS 6.11，PRD-SIG-006「長期驗證（LTV）與時間戳
// 伺服器」的 LT 層級）。
//
// ISO 32000-2 §12.8.4.1：/Root /DSS 帶 /Certs /CRLs /OCSPs 三個串流物件陣列，
// 與 /VRI 字典——鍵是每個簽章 /Contents 的 SHA-1 雜湊（大寫十六進位字串），
// 值是那個簽章驗證當下用得到的證據子集。有了它，驗證端不必在憑證過期或
// CRL/OCSP 責任者下線之後才發現查不到吊銷狀態；沒有 /DSS 的簽章只能查詢
// 「現在」的吊銷狀態，而簽署當下有效、簽署後被吊銷或憑證過期的情境完全
// 無法回溯判斷，這正是 R1 驗證側已知的邊界（見 pkcs7_verifier.h 開頭）。
//
// 走物件層通道（ADR-002），純附加：既有簽章與內容一個位元組都不動，只新增
// 物件。附加本身會讓 Acrobat 顯示「簽章後有變更」，這是增量儲存的本質，
// PRD-SIG-003 承諾的是「不變成無效」而不是「不顯示變更」——LTV 證據的附加
// 不是例外。
//
// 已知限制（未對照 Acrobat 實機驗證，交付報告已列出）：
//   - /VRI 鍵的雜湊輸入是「/Contents 十六進位字串解碼後的完整位元組，含尾端
//     補零」，不是去除補零後的實際 DER 長度。ETSI 規格文字對這一點描述不夠
//     精確，PDFBox／iText 兩個開源實作彼此也不一致；這裡選擇「不需要重新
//     剖析 DER 長度就能算出」的版本，若與 Acrobat 實測不符，這是第一個該
//     檢查的假設。呼叫端必須自行決定要傳哪一種位元組進 vriKeyForContents。
//   - 不處理「文件已經有 /DSS，這次要合併而不是新建」的情境；重複呼叫會
//     在目錄寫入第二個 /DSS 參照，覆蓋掉前一個而不是合併證據池。多次 LTV
//     更新（Acrobat 稱為 DSS 的漸進式擴充）需要另外的合併邏輯，這裡沒有做。
//   - 不寫 /Extensions /ESIC；多數實作在沒有它時仍會嘗試解析 /DSS，
//     但嚴格遵循規格的驗證器可能忽略。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::signature {

// 單一簽章可回溯驗證所需的證據。certs/crls/ocspResponses 都是原始 DER。
struct SignatureEvidence {
    std::string sha1OfContentsHex;  // /VRI 的鍵；由 vriKeyForContents() 產生
    std::vector<std::vector<std::uint8_t>> certs;
    std::vector<std::vector<std::uint8_t>> crls;
    std::vector<std::vector<std::uint8_t>> ocspResponses;
};

struct DssBuildOptions {
    // 文件層級共用的證據（例如所有簽章共用同一張根 CA、同一份 CRL）。
    // 這些一律寫進頂層 /Certs /CRLs /OCSPs 陣列，但不出現在任何 /VRI 項裡——
    // 若某個簽章也需要它，必須連同其餘證據一起放進 perSignature 對應的欄位，
    // 這裡不做「猜測哪些共用證據屬於哪個簽章」的推斷。
    std::vector<std::vector<std::uint8_t>> sharedCerts;
    std::vector<std::vector<std::uint8_t>> sharedCrls;
    std::vector<std::vector<std::uint8_t>> sharedOcspResponses;

    std::vector<SignatureEvidence> perSignature;
};

struct DssBuildResult {
    bool ok{false};
    std::string diagnostic;
    int dssObject{0};
    std::size_t certCount{0};  // 去重後實際寫出的串流物件數
    std::size_t crlCount{0};
    std::size_t ocspCount{0};
};

// 附加 /DSS 到目錄。appender 必須已經 open() 一份文件（不要求一定已有簽章：
// 這裡只負責寫證據存放區本身，簽章是否存在由呼叫端的流程保證）。
[[nodiscard]] DssBuildResult appendDocumentSecurityStore(objects::IncrementalAppender& appender,
                                                         const DssBuildOptions& options);

// 由「/Contents 十六進位字串解碼後的原始位元組」算出 /VRI 鍵（SHA-1，
// 大寫十六進位，40 字元）。見標頭已知限制第一條。
[[nodiscard]] std::string vriKeyForContents(const std::vector<std::uint8_t>& contentsRawBytes);

}  // namespace alioth::engine::signature
