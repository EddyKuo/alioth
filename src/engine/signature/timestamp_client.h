#pragma once

// RFC 3161 時間戳用戶端（WBS 6.11 的一部分，PRD-SIG-006「長期驗證（LTV）與
// 時間戳伺服器」）。
//
// 分工與 revocation.h 的 OcspRevocationChecker 完全對稱，理由也相同：
//   - 協定（TimeStampReq 組裝、TimeStampResp 解析）在引擎轉接層，三平台共用
//   - 傳輸（HTTP／HTTPS）不在這裡。網路存取屬於平台層，「簽署一份文件會不會
//     自動連外要求時間戳」是使用者必須能控制的隱私與合規決策，不該由引擎
//     自作主張連上一個寫死的 URL
//
// 因此 TimestampRequester 需要呼叫端注入 Transport；CI 測試一律餵本地產生的
// TimeStampResp 位元組，不連任何真實 TSA（見 tests/ltv 的說明）。
//
// 訊息摘要（messageImprint）在 PAdES-B-T 的語境下永遠是「簽章值本身」的雜湊
// （RFC 5126 CAdES-T，ETSI EN 319 122-1 附加時間戳的慣例），不是被簽內容的
// 雜湊——這一點容易搞混，混錯的後果是時間戳在密碼學上完全有效，但驗證端
// 拿被簽內容重算摘要去比對時永遠對不上，症狀會被誤診成時間戳本身壞掉。
// 呼叫端負責算出這個雜湊（signature_creator.cpp 從 CMS_SignerInfo 的簽章值
// 取出），這裡只管 TSA 協定本身。
//
// 目前只支援 SHA-256 訊息摘要演算法：PAdES 建議演算法與本專案簽章本身用的
// 摘要演算法一致（見 signature_creator.cpp 的 EVP_sha256），沒有理由讓時間戳
// 摘要用不同演算法徒增一種要測試的排列組合。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace alioth::engine::signature {

struct TimestampRequestOptions {
    // 是否要求 TSA 在回應裡附上自己的簽署憑證（TimeStampReq 的 certReq）。
    // 開啟後驗證端不需要另外取得 TSA 憑證就能驗時間戳本身——這與
    // SigningIdentity 把中介鏈一併內嵌是同一個「驗證端不必另外要件」的立場。
    bool requestCertificates{true};

    // 隨機數，防止回應被重放到另一次請求上。多數公開 TSA 會在回應裡原樣
    // 附回，驗證端理論上應該比對，但本專案目前不做這一步比對
    // （見 timestamp_client.cpp 的已知限制）。
    bool includeNonce{true};
};

struct TimestampResult {
    bool ok{false};
    std::string diagnostic;  // 失敗原因；成功時為空

    // TimeStampToken 的 DER：一個 ContentInfo（signed-data），可以直接嵌進
    // CMS 的 unsigned attribute，也可以獨立用 d2i_PKCS7 解析。
    std::vector<std::uint8_t> tokenDer;

    std::int64_t genTimeUnix{0};  // TSTInfo 的 genTime，Unix 秒；解析失敗為 0
    std::string serialHex;        // TSTInfo 的序號
};

// 建立 TimeStampReq 的 DER。純函數，不碰網路，方便單獨測試請求格式是否合法。
[[nodiscard]] std::vector<std::uint8_t> buildTimeStampRequestDer(
    const std::vector<std::uint8_t>& messageImprintSha256, const TimestampRequestOptions& options,
    std::string* diagnostic);

// 解析 TimeStampResp 的 DER。純函數，不碰網路。
//
// 只接受 TS_STATUS_GRANTED 與 TS_STATUS_GRANTED_WITH_MODS 兩種狀態為成功；
// 其餘（拒絕、等候、撤銷警示／通知）一律回報失敗，即使回應本身格式合法——
// 「TSA 回話了」與「TSA 給了可用的時間戳」是兩件事，混為一談會讓一個
// 被拒絕的請求看起來像是簽出了 PAdES-T。
[[nodiscard]] TimestampResult parseTimeStampResponseDer(const std::vector<std::uint8_t>& responseDer);

class TimestampRequester {
public:
    // 回傳空向量代表傳輸失敗（逾時、連線被拒、使用者已關閉外連）。
    // 與 OcspRevocationChecker::Transport 同一個型別形狀，讓呼叫端能共用
    // 同一套「使用者尚未同意連網就回空」的守門邏輯。
    using Transport = std::function<std::vector<std::uint8_t>(
        const std::string& url, const std::vector<std::uint8_t>& derRequest)>;

    explicit TimestampRequester(Transport transport);

    // 組請求、送出、解析回應，三步合一。tsaUrl 由呼叫端提供
    // （來自簽署身分的設定或使用者輸入，不在這裡寫死任何預設 TSA）。
    [[nodiscard]] TimestampResult requestToken(const std::string& tsaUrl,
                                               const std::vector<std::uint8_t>& messageImprintSha256,
                                               const TimestampRequestOptions& options = {}) const;

private:
    Transport transport_;
};

}  // namespace alioth::engine::signature
