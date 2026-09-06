#pragma once

// 從網頁 URL 建立 PDF（PRD-IO-014）。
//
// **範圍見 domain/document_source.h 的降級說明（唯一真相來源，這裡不重複）**：
// 這不是網頁排版引擎，是「下載 → 去標籤取純文字 → 走既有的純文字排版」。
// 完整的 HTML/CSS 渲染需要 Qt WebEngine，已評估並回報 BLOCKED
// （超出安裝包上限、與零腳本執行的安全立場衝突），見 WP35 交付報告。
//
// 傳輸注入與 URL／大小／內容類型驗證的分工原則與 url_document_source.h
// 完全一致（那三條理由在這裡逐字成立，不重複列出）。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "domain/document_source.h"
#include "engine/create/text_to_pdf.h"
#include "engine/create/url_document_source.h"  // 共用 HttpResponse

namespace alioth::engine::create {

using WebPageFetcher = std::function<HttpResponse(const std::string& url, std::uint64_t maxBytes)>;

struct WebPageImportResult {
    bool ok{false};
    domain::create::HtmlRejection rejection{domain::create::HtmlRejection::None};
    std::string diagnostic;
    std::string bytes;
    std::string pageTitle;
    std::size_t pageCount{0};
};

class WebPageToPdfConverter {
public:
    explicit WebPageToPdfConverter(WebPageFetcher fetcher = {},
                                   domain::create::HtmlFetchPolicy policy = {});

    void setPolicy(domain::create::HtmlFetchPolicy policy) { policy_ = policy; }
    [[nodiscard]] const domain::create::HtmlFetchPolicy& policy() const noexcept { return policy_; }

    // 只驗網址，不連網。與 UrlDocumentOpener::precheck 同樣的理由：UI 在跳出
    // 「要下載嗎」之前就該先擋掉不支援的結構描述。
    [[nodiscard]] domain::create::HtmlRejection precheck(const std::string& url) const;

    [[nodiscard]] WebPageImportResult convert(const std::string& url,
                                              const domain::create::TextImportOptions& textOptions = {});

private:
    WebPageFetcher fetcher_;
    domain::create::HtmlFetchPolicy policy_;
};

}  // namespace alioth::engine::create
