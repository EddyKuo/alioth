#pragma once

// 建立數位簽章（WBS 6.11，PRD-SIG-004「建立數位簽章（PAdES-B）」與
// PRD-SIG-007「認證文件（Certify，含無實體簽名）」）。
//
// 這個函式只做一件事：把一個新的 /Sig 簽章欄位以增量附加寫進檔尾，簽章涵蓋
// 除了它自己 /Contents 之外的整份檔案。它刻意不碰任何既有內容——原檔一個
// 位元組都不會被改動，這與 PRD-SIG-003（增量儲存不破壞既有簽章）是同一個
// 技術基礎的兩個方向：那邊保證「加註解不弄壞既有簽章」，這裡保證
// 「加簽章不弄壞既有內容」。
//
// /ByteRange 與 /Contents 的雞生蛋問題：簽章涵蓋的位元組範圍要等到整份輸出
// 序列化完成、量出實際位移之後才知道，但序列化完成之後又不能再改動任何
// 位元組的寬度——改寬度會讓後面所有物件的位移全部錯位。解法是先用等寬的
// 佔位符（全零 /Contents、固定十位數的 /ByteRange 數字）序列化一次，量出
// 佔位符實際落在檔案的哪個位置，再原地覆寫成等寬的最終值。這正是
// tests/signature/signature_fixture.h 手工造測試語料時用的同一招，
// 這裡用 IncrementalAppender 走正式的物件層通道重做一次。
//
// PAdES-B-B 基準（ISO 32000-2 §12.8、ETSI EN 319 122-1）目前落實的部分：
//   - /SubFilter ETSI.CAdES.detached
//   - CMS SignedData（detached），簽署憑證與中介鏈一併內嵌
//   - signed attribute id-aa-signingCertificateV2（ESS，RFC 5035），
//     防止「換一張序號相同雜湊不同的憑證」的替換攻擊
// 尚未落實（見交付報告 BLOCKED 清單）：
//   - 可見簽章外觀（本函式只產生無實體 Widget，/Rect 全零、無 /AP）
//
// 時間戳記（PRD-SIG-006，PAdES-B-T）現在是選用項：CreateSignatureOptions::
// timestamp 開啟時，會在 CMS_final 之後、序列化最終 DER 之前，向 TSA 索取一份
// 涵蓋簽章值本身的 RFC 3161 時間戳，並以 unsigned attribute
// （id-aa-signatureTimeStampToken）掛進同一份 CMS。刻意在「同一次」序列化裡
// 做完，而不是簽完再另外補一次增量儲存：/Contents 的保留寬度（reserveBytes）
// 一開始就要算進時間戳的大小，事後補時間戳會有「新的 DER 放不放得進當初
// 保留的寬度」這個不確定性，在這裡直接讓呼叫端一次决定好保留多少空間更穩妥。
// LTV／DSS（/Root /DSS）是文件層級的證據存放區，不屬於單一簽章物件，因此
// 在 dss_builder.h 獨立成另一個函式，通常在完成簽署（含時間戳）後另外呼叫。
//
// 只能對未加密文件操作（與其餘物件層寫入通道一致，ADR-002 驗收條件 5）。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/signature/pkcs7_verifier.h"
#include "engine/signature/signature_appearance.h"
#include "engine/signature/signing_identity.h"
#include "engine/signature/timestamp_client.h"

namespace alioth::engine::signature {

enum class CertifyLevel {
    None,                       // 一般簽章（PRD-SIG-004），不寫 DocMDP
    NoChangesAllowed,           // DocMDP /P 1：簽署後不允許任何變更
    FormFillingAllowed,         // DocMDP /P 2：僅允許表單填寫
    FormFillingAndAnnotations,  // DocMDP /P 3：允許表單填寫與註解
};

// PRD-SIG-006 的時間戳部分。tsaUrl 與 transport 都由呼叫端提供：本函式
// 不寫死任何預設 TSA，網路存取與否是使用者的隱私決策（見 timestamp_client.h）。
struct TimestampOptions {
    bool enabled{false};
    std::string tsaUrl;
    TimestampRequester::Transport transport;
    TimestampRequestOptions requestOptions;
};

struct CreateSignatureOptions {
    std::string reason;
    std::string location;
    std::string contactInfo;
    std::string fieldName{"Signature1"};  // AcroForm 欄位 /T；多重簽署時必須各自唯一
    int pageIndex{0};                     // 簽章欄位掛在哪一頁；只影響 /Widget 的 /P
    int reserveBytes{8192};               // /Contents 預留位元組數（DER 長度上限）

    TimestampOptions timestamp;

    CertifyLevel certify{CertifyLevel::None};
    // 認證簽章（Certify）依規格必須是文件的第一個簽章（ISO 32000-2 §12.8.2.2）；
    // 對已簽章文件再蓋一個 DocMDP 是無意義的（第二個簽章不會被當成 Certifying
    // Signature）。呼叫端必須自行確認（例如先用 SignatureScanner::signatureCount()
    // 查詢）並在這裡明講——這個類別刻意只在位元組層次運作，不重新開一份 PDFium
    // 文件把手去自己掃描，那樣會讓它没辦法脫離 GUI／PDFium 執行緒單獨測試。
    bool documentAlreadyHasSignatures{false};

    // 可見簽章外觀（PRD-SIG-004 的補完，PRD-SIG-005 的前置條件，見 ADR-004）。
    // rectPt 為空矩形時維持無實體簽章（/Rect 全零、無 /AP）——那在密碼學上
    // 完全有效，只是畫面上看不見。
    SignatureAppearanceOptions appearance;

    // 簽署時間。0 代表使用系統當下時間。開放這個參數是為了讓測試能固定時間，
    // 而不必等待或竄改系統時鐘。
    std::int64_t signingTimeUnix{0};
};

struct CreateSignatureResult {
    bool ok{false};
    std::string diagnostic;  // 失敗原因；成功時為空
    std::string bytes;       // 完整輸出檔（原檔前綴 + 附加段），失敗時為空
    std::uint64_t appendedBytes{0};
    int signatureFieldObject{0};  // /Widget 註解物件編號
    int signatureDictObject{0};   // /Sig 字典物件編號

    // 最終寫入的 /ByteRange 四個數值（依序：起點, 第一段長度, 第二段起點,
    // 第二段長度）。連同下面兩個欄位一起公開，是為了讓批次簽署／自我驗證
    // （signature_creator.cpp 的 verifyCreatedSignature）不必重新剖析 bytes
    // 找出 /Contents 在哪裡——那個剖析在 createSignature 內部已經做過一次，
    // 沒有理由讓每個呼叫端都重做一遍容易寫錯的字串搜尋。
    std::int64_t byteRange[4]{0, 0, 0, 0};

    // 是否已嵌入 RFC 3161 時間戳（PAdES-B-T）。timestamp.enabled 時若嵌入失敗，
    // 整個 createSignature 直接回報 ok=false（診斷寫進 diagnostic）而不是靜默
    // 退回一份沒有時間戳的簽章——呼叫端明確要求時間戳，拿到的東西卻沒有，
    // 是「看起來對但實際不符期待」的一種，IL-4 不允許。
    bool timestamped{false};
};

[[nodiscard]] CreateSignatureResult createSignature(const std::string& sourceBytes,
                                                     const SigningIdentity& identity,
                                                     const CreateSignatureOptions& options);

// ---------------------------------------------------------------------------
// 批次簽署（PRD-SIG-005「多頁批次簽章」的共同基礎）。
//
// PRD 表格對這條需求只有一行「多頁批次簽章」，沒有敘述文字可以判斷「批次」
// 指的是「一次對多份文件各簽一次」還是「同一份文件在多個簽章欄位依序簽署」
// ——兩者的呼叫端組裝方式與 UI 意涵完全不同，語意本身見
// exceptions/EXC_20260906_RD_SA_wp38_sig005_semantics.md 的 BLOCKED。
//
// 這裡先做兩種解讀都要用得到的共同基礎：一次載入身分、對一組
// {來源位元組, 簽章選項} 逐一呼叫 createSignature()、逐一以本專案的驗證側
// （pkcs7_verifier）自我核對剛簽出來的位元組、彙整成一份批次報告。
//   - 若語意裁定為「多份文件」：呼叫端把每份文件各自的位元組放進
//     BatchSignItem::sourceBytes。
//   - 若語意裁定為「同一份文件的多個欄位依序簽署」：呼叫端把上一步的
//     result.bytes 當成下一步的 sourceBytes，並給不同的 fieldName——
//     這正是 documentAlreadyHasSignatures 已經支援的情境（Certify 例外）。
// 兩種情境都不需要改動 signBatch 本身，差別只在呼叫端怎麼組 items。
// ---------------------------------------------------------------------------

struct BatchSignItem {
    std::string sourceBytes;
    CreateSignatureOptions options;
};

struct BatchSignItemResult {
    CreateSignatureResult create;

    // 自我驗證：用本專案的 pkcs7_verifier 對剛簽出來的位元組重新驗證一次，
    // 不依賴 PDFium／SignatureScanner（那條路徑需要非同步的專用執行緒，
    // 批次簽署刻意保持同步呼叫）。trust 為 nullptr 時不驗證，
    // verifyAttempted 維持 false。
    bool verifyAttempted{false};
    SignatureReport verify;
};

struct BatchSignResult {
    std::vector<BatchSignItemResult> items;

    [[nodiscard]] std::size_t successCount() const noexcept {
        std::size_t count = 0;
        for (const BatchSignItemResult& item : items) count += item.create.ok ? 1 : 0;
        return count;
    }
};

// trust 非 nullptr 時，每一份簽出來的文件都會立刻自我驗證一次；
// revocation 可為 nullptr（等同 RevocationPolicy::Skip 的效果由 verifyOptions
// 自行控制，這裡不強制覆寫呼叫端的政策設定）。
[[nodiscard]] BatchSignResult signBatch(const SigningIdentity& identity,
                                        const std::vector<BatchSignItem>& items,
                                        const TrustStore* trust = nullptr,
                                        const VerifyOptions& verifyOptions = {},
                                        RevocationChecker* revocation = nullptr);

}  // namespace alioth::engine::signature
