#pragma once

// 從 URL 開啟文件（PRD-IO-010，WBS 15）。
//
// 分工與 engine/signature/revocation.h 的 OCSP 查詢完全一致，理由也一樣：
// 協定與策略在引擎轉接層、傳輸由呼叫端注入。
//
//   - 網路存取屬於平台層。引擎層自己連外會讓「開一份文件會不會連網」
//     這件使用者必須能控制的隱私決策，變成引擎的內部實作細節
//   - 沒有注入 fetcher 時回傳 TransportFailed 而不是靜默成功。把「沒下載」
//     講成「沒問題」正是三態模型要防的那種事
//   - 測試不該依賴網路。注入式設計讓逾時、413、偽造的 Content-Type、
//     半截檔案這些情境全部可以在單元測試裡重現，而真的連網只能重現
//     「今天剛好能不能連上」
//
// 一律先完整下載到暫存檔，再交給引擎開啟。不邊下載邊解析的理由有兩個：
// PDF 的 xref 在檔尾，串流解析註定要來回跳；而且 FPDF_LoadCustomDocument
// 的讀取回呼是同步的，掛在網路上等於讓 PDFium 的執行緒被 RTT 卡住。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "domain/document_source.h"

namespace alioth::engine::create {

struct HttpResponse {
    bool transportOk{false};  // false 代表連不上、逾時、TLS 失敗
    int statusCode{0};
    std::string contentType;
    std::uint64_t declaredLength{0};  // Content-Length；0 代表伺服器沒給
    std::string body;
};

// maxBytes 是給傳輸端的提示：超過就該中止，不要先收完再讓上層丟掉。
// 上層仍會自己再驗一次——提示不是保證。
using HttpFetcher = std::function<HttpResponse(const std::string& url, std::uint64_t maxBytes)>;

struct UrlOpenResult {
    bool ok{false};
    domain::create::UrlRejection rejection{domain::create::UrlRejection::None};
    std::string diagnostic;

    // 下載完成的暫存檔路徑。呼叫端負責在關閉文件後刪除；本類別提供
    // discard() 但不自動清理——引擎不知道文件會被開多久。
    std::filesystem::path temporaryPath;
    std::uint64_t byteCount{0};
};

class UrlDocumentOpener {
public:
    explicit UrlDocumentOpener(HttpFetcher fetcher = {},
                               domain::create::UrlFetchPolicy policy = {});

    void setPolicy(domain::create::UrlFetchPolicy policy) { policy_ = policy; }
    [[nodiscard]] const domain::create::UrlFetchPolicy& policy() const noexcept { return policy_; }

    // 暫存目錄。預設是系統暫存目錄；測試會指到自己的目錄，避免在 CI 上
    // 留下沒人負責清掉的檔案。
    void setTemporaryDirectory(std::filesystem::path directory);

    // 只驗網址，不連網。UI 在跳出「要下載嗎」之前就該先擋掉 file:// 這類
    // 結構描述——問完再拒絕只是多浪費使用者一次點擊。
    [[nodiscard]] domain::create::UrlRejection precheck(const std::string& url) const;

    [[nodiscard]] UrlOpenResult open(const std::string& url);

    static void discard(const UrlOpenResult& result) noexcept;

private:
    HttpFetcher fetcher_;
    domain::create::UrlFetchPolicy policy_;
    std::filesystem::path temporaryDirectory_;
    std::uint64_t sequence_{0};
};

}  // namespace alioth::engine::create
