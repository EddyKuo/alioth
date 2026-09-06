#include "engine/create/url_document_source.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace alioth::engine::create {

using domain::create::UrlRejection;

namespace {

std::string hashUrl(const std::string& url, std::uint64_t salt) {
    std::uint64_t hash = 1469598103934665603ULL ^ salt;
    for (const unsigned char c : url) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (int i = 15; i >= 0; --i) hex.push_back(digits[(hash >> (i * 4)) & 0xF]);
    return hex;
}

UrlOpenResult reject(UrlRejection reason, std::string extra = {}) {
    UrlOpenResult result;
    result.rejection = reason;
    result.diagnostic = domain::create::describeUrlRejection(reason);
    if (!extra.empty()) result.diagnostic += "（" + extra + "）";
    return result;
}

}  // namespace

UrlDocumentOpener::UrlDocumentOpener(HttpFetcher fetcher, domain::create::UrlFetchPolicy policy)
    : fetcher_(std::move(fetcher)), policy_(policy) {
    std::error_code ec;
    temporaryDirectory_ = std::filesystem::temp_directory_path(ec);
    if (ec) temporaryDirectory_ = std::filesystem::path{"."};
}

void UrlDocumentOpener::setTemporaryDirectory(std::filesystem::path directory) {
    temporaryDirectory_ = std::move(directory);
}

UrlRejection UrlDocumentOpener::precheck(const std::string& url) const {
    return domain::create::validateUrl(url, policy_);
}

UrlOpenResult UrlDocumentOpener::open(const std::string& url) {
    const UrlRejection urlProblem = domain::create::validateUrl(url, policy_);
    if (urlProblem != UrlRejection::None) return reject(urlProblem);

    if (!fetcher_) {
        // 沒有注入傳輸就不會有任何連線。這裡明確失敗，而不是回一個空文件。
        return reject(UrlRejection::TransportFailed, "未注入 HTTP 傳輸實作");
    }

    const HttpResponse response = fetcher_(url, policy_.maxBytes);
    if (!response.transportOk) return reject(UrlRejection::TransportFailed);
    if (response.statusCode != 200) {
        return reject(UrlRejection::HttpStatusNotOk, "狀態碼 " + std::to_string(response.statusCode));
    }
    if (response.declaredLength > policy_.maxBytes) {
        return reject(UrlRejection::DeclaredSizeTooLarge,
                      std::to_string(response.declaredLength) + " > " +
                          std::to_string(policy_.maxBytes));
    }

    const UrlRejection typeProblem =
        domain::create::validateContentType(response.contentType, policy_);
    if (typeProblem != UrlRejection::None) return reject(typeProblem, response.contentType);

    const UrlRejection bodyProblem = domain::create::validateBody(
        reinterpret_cast<const std::uint8_t*>(response.body.data()), response.body.size(),
        policy_);
    if (bodyProblem != UrlRejection::None) return reject(bodyProblem);

    std::error_code ec;
    std::filesystem::create_directories(temporaryDirectory_, ec);

    const std::filesystem::path path =
        temporaryDirectory_ / ("alioth-url-" + hashUrl(url, sequence_++) + ".pdf");

    // 二進位模式是必要條件不是偏好：文字模式在 Windows 上會把 0x0A 展開成
    // 0x0D 0x0A，PDF 裡的每一個換行位元組都會偏移，xref 的位移全部失效。
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return reject(UrlRejection::TransportFailed, "無法建立暫存檔");
    out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    out.flush();
    if (!out) {
        out.close();
        std::filesystem::remove(path, ec);
        return reject(UrlRejection::TransportFailed, "暫存檔寫入失敗");
    }
    out.close();

    UrlOpenResult result;
    result.ok = true;
    result.temporaryPath = path;
    result.byteCount = response.body.size();
    return result;
}

void UrlDocumentOpener::discard(const UrlOpenResult& result) noexcept {
    if (result.temporaryPath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(result.temporaryPath, ec);
}

}  // namespace alioth::engine::create
