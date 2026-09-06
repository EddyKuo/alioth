#include "engine/create/web_page_to_pdf.h"

#include <utility>

namespace alioth::engine::create {

using domain::create::HtmlRejection;

namespace {

WebPageImportResult reject(HtmlRejection reason, std::string extra = {}) {
    WebPageImportResult result;
    result.rejection = reason;
    result.diagnostic = domain::create::describeHtmlRejection(reason);
    if (!extra.empty()) result.diagnostic += "（" + extra + "）";
    return result;
}

}  // namespace

WebPageToPdfConverter::WebPageToPdfConverter(WebPageFetcher fetcher,
                                             domain::create::HtmlFetchPolicy policy)
    : fetcher_(std::move(fetcher)), policy_(policy) {}

HtmlRejection WebPageToPdfConverter::precheck(const std::string& url) const {
    return domain::create::validateHtmlUrl(url, policy_);
}

WebPageImportResult WebPageToPdfConverter::convert(
    const std::string& url, const domain::create::TextImportOptions& textOptions) {
    const HtmlRejection urlProblem = domain::create::validateHtmlUrl(url, policy_);
    if (urlProblem != HtmlRejection::None) return reject(urlProblem);

    if (!fetcher_) return reject(HtmlRejection::TransportFailed, "未注入 HTTP 傳輸實作");

    const HttpResponse response = fetcher_(url, policy_.maxBytes);
    if (!response.transportOk) return reject(HtmlRejection::TransportFailed);
    if (response.statusCode != 200) {
        return reject(HtmlRejection::HttpStatusNotOk, "狀態碼 " + std::to_string(response.statusCode));
    }
    if (response.declaredLength > policy_.maxBytes) {
        return reject(HtmlRejection::DeclaredSizeTooLarge);
    }
    const HtmlRejection typeProblem =
        domain::create::validateHtmlContentType(response.contentType, policy_);
    if (typeProblem != HtmlRejection::None) return reject(typeProblem, response.contentType);
    if (response.body.empty()) return reject(HtmlRejection::EmptyBody);
    if (response.body.size() > policy_.maxBytes) return reject(HtmlRejection::BodyTooLarge);

    const domain::create::HtmlExtraction extraction = domain::create::extractReadableText(response.body);
    if (!extraction.ok) return reject(HtmlRejection::NoExtractableText, extraction.diagnostic);

    // 標題另起一段放在最前面，讓使用者至少能從第一頁認出這是哪個網頁的存檔。
    std::string fullText = extraction.title.empty()
                                ? extraction.bodyText
                                : extraction.title + "\n\n" + extraction.bodyText;

    const TextImportResult textResult = createPdfFromPlainText(fullText, textOptions);
    if (!textResult.ok) {
        WebPageImportResult result;
        result.diagnostic = "文字排版失敗（可能含 CJK 或其他非 ASCII 字元）：" + textResult.diagnostic;
        return result;
    }

    WebPageImportResult result;
    result.ok = true;
    result.bytes = textResult.bytes;
    result.pageTitle = extraction.title;
    result.pageCount = textResult.pageCount;
    return result;
}

}  // namespace alioth::engine::create
