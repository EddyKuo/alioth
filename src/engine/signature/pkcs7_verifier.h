#pragma once

// PKCS#7 / CMS 簽章驗證（WBS 6.6，PRD-SIG-001）。
//
// 這是本工作包最重要的可測試邊界：函式輸入是「被簽章涵蓋的位元組」與
// 「PKCS#7 blob」，輸出是三態報告。它不碰 PDFium、不碰檔案、不碰執行緒，
// 因此可以用手工組出的憑證與 blob 逐個情境驗證，
// 包括正例、改一個位元組、憑證過期、憑證鏈不受信任、憑證被吊銷。
//
// PDFium 與 OpenSSL 的分工（PRD 附錄 C）：PDFium 只負責列舉簽章、交出
// /ByteRange 與 /Contents；解析、憑證鏈、信任存放區、吊銷查詢全在這裡，
// 三平台共用同一份實作——這正是 PRD §4.1 保留 OpenSSL 的理由。
//
// 已知邊界：憑證鏈的有效期判定使用系統當下時間，不接受注入時間。
// 要以「簽署當下」為基準判定，需要可信時間戳（PAdES LTV），那是 R3 的 PRD-SIG-006。
//
// PRD-SIG-006 現況：本檔會偵測、解析並回報 unsigned attribute
// id-aa-signatureTimeStampToken 是否存在與它的 genTime（SignatureReport::
// hasTimestamp / timestampGenTimeUnix），但**不**驗證那個時間戳權杖本身的
// 簽章（需要 TSA 的信任錨，而 TrustStore 目前只承載簽署鏈的信任錨——見
// 交付報告已知限制）、也**不**把 timestampGenTimeUnix 接進 classify() 或
// certificateExpired／certificateNotYetValid 的判斷。也就是說，「簽署時
// 憑證有效但簽署後過期」目前仍然只能看到系統當下時間判出的過期結果，
// 即使檔案裡確實有一個誠實的時間戳可以證明簽署當下有效。這是刻意保守的
// 半步：先讓資訊「看得見」，把它接進判色邏輯留給下一個工作包，因為那牽涉
// 到要不要新增第四種燈號語意（「簽署時有效，現在過期」），屬於 UI 與 PM
// 都該參與的判斷，不是驗證側能單方面決定的事。

#include <cstdint>
#include <vector>

#include "engine/signature/revocation.h"
#include "engine/signature/signature_types.h"
#include "engine/signature/trust_store.h"

namespace alioth::engine::signature {

// signedBytes 為 /ByteRange 串接後的位元組；pkcs7Der 為 /Contents 去除補零後的
// DER 位元組；coverage 是那段位元組的涵蓋範圍檢查結果。revocation 可為 nullptr。
//
// coverage 刻意是必填參數而不是選填：涵蓋範圍不完整的簽章在密碼學上完全有效，
// 忘記傳就會產生一個「有效」的綠燈，正好是簽章偽造想要的結果。
// 非 PDF 的呼叫端請明示傳入 coverageOfWholeInput()。
//
// 回傳的報告已經過 finalize()，trust 欄位可直接用。
[[nodiscard]] SignatureReport verifyDetachedPkcs7(const std::vector<std::uint8_t>& signedBytes,
                                                  const std::vector<std::uint8_t>& pkcs7Der,
                                                  const ByteRangeCheck& coverage,
                                                  const TrustStore& trust,
                                                  const VerifyOptions& options,
                                                  RevocationChecker* revocation = nullptr);

// /Contents 是固定寬度的十六進位字串，簽章值之後補零到預留長度。
// DER 的長度自己寫在標頭裡，所以可以精確裁掉補零；
// 不裁的話 d2i_PKCS7 會因為尾端多餘位元組而失敗——那會被誤診成「簽章損毀」。
[[nodiscard]] std::vector<std::uint8_t> trimDerPadding(const std::vector<std::uint8_t>& contents);

}  // namespace alioth::engine::signature
