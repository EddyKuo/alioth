#include "engine/compare/duplicate_pages.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "domain/text_layer.h"
#include "engine/compare/utf8_scan.h"

namespace alioth::engine::compare {
namespace {

// 量化後的尺寸鍵。用整數當鍵是必要的：浮點數當 map 的鍵，
// 兩個「看起來一樣」的尺寸會落在不同的桶裡，症狀是真重複頁被漏報。
using SizeKey = std::pair<std::int64_t, std::int64_t>;

[[nodiscard]] SizeKey quantize(const domain::SizeF& size, std::int32_t quantumTenths) {
    const double quantum = static_cast<double>(std::max(quantumTenths, 1)) / 10.0;
    return SizeKey{static_cast<std::int64_t>(std::llround(size.width / quantum)),
                   static_cast<std::int64_t>(std::llround(size.height / quantum))};
}

}  // namespace

std::string normalizeForFingerprint(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    bool pendingSpace = false;
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        char32_t c = 0;
        const std::size_t consumed = decodeUtf8(utf8, pos, c);
        if (consumed == 0) break;
        pos += consumed;
        const domain::CharCategory category = domain::categorize(c);
        if (category == domain::CharCategory::Whitespace ||
            category == domain::CharCategory::Control) {
            // 空白只在已經有內容之後才記錄，開頭的空白因此自然被丟掉。
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        domain::appendUtf8(out, c);
    }
    return out;
}

std::vector<DuplicatePageGroup> findDuplicatePages(std::span<const PageFingerprintInput> pages,
                                                   const DuplicateOptions& options) {
    // 以 map 而不是 unordered_map：鍵含字串與兩個整數，
    // 用有序容器可以讓輸出順序穩定，測試才有辦法逐項比對。
    std::map<std::tuple<std::string, std::int64_t, std::int64_t>, DuplicatePageGroup> groups;

    for (const PageFingerprintInput& page : pages) {
        std::string normalized = normalizeForFingerprint(page.text);
        if (normalized.empty() && !options.includeTextlessPages) continue;
        const SizeKey key = quantize(page.sizePt, options.sizeQuantumTenths);
        auto [it, inserted] =
            groups.try_emplace(std::tuple{normalized, key.first, key.second}, DuplicatePageGroup{});
        if (inserted) {
            it->second.sizePt = page.sizePt;
            it->second.normalizedText = std::move(normalized);
        }
        it->second.pages.push_back(page.pageIndex);
    }

    std::vector<DuplicatePageGroup> result;
    for (auto& [key, group] : groups) {
        if (group.pages.size() < 2) continue;
        std::sort(group.pages.begin(), group.pages.end());
        result.push_back(std::move(group));
    }
    std::sort(result.begin(), result.end(),
              [](const DuplicatePageGroup& l, const DuplicatePageGroup& r) {
                  return l.pages.front() < r.pages.front();
              });
    return result;
}

}  // namespace alioth::engine::compare
