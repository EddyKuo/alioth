#include "engine/redaction/find_and_redact.h"

#include <utility>
#include <vector>

#include "engine/text/text_extractor.h"
#include "engine/text/text_search.h"

namespace alioth::engine::redaction {
namespace {

domain::RectF padded(const domain::RectF& box, double padding) {
    return domain::RectF{box.left - padding, box.bottom - padding, box.right + padding,
                         box.top + padding};
}

}  // namespace

FindRedactResult findAndMarkRedactions(const std::string& path, const std::string& password,
                                       const std::string& queryUtf8,
                                       const FindRedactOptions& options) {
    FindRedactResult result;
    if (queryUtf8.empty()) {
        result.diagnostic = "搜尋字串為空";
        return result;
    }

    text::TextExtractor extractor;
    domain::DocumentError openError = domain::DocumentError::Unknown;
    extractor.open(path, password, [&openError](domain::DocumentError error) { openError = error; });
    extractor.waitForIdle();
    if (openError != domain::DocumentError::None) {
        result.diagnostic = describe(openError);
        return result;
    }

    text::SearchOptions searchOptions;
    searchOptions.matchCase = options.matchCase;
    searchOptions.matchWholeWord = options.matchWholeWord;
    searchOptions.contextRadius = 0;

    const std::int32_t pageCount = extractor.pageCount();
    for (std::int32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        if (result.truncated) break;
        extractor.withTextPage(pageIndex, [&](const text::TextPage* page) {
            if (page == nullptr) return;
            const std::vector<domain::SearchResult> matches =
                text::searchPage(*page, queryUtf8, searchOptions);
            for (const domain::SearchResult& match : matches) {
                if (result.marks.size() >= static_cast<std::size_t>(options.maxMarks)) {
                    result.truncated = true;
                    return;
                }
                ++result.matchCount;

                domain::RedactionMark mark;
                mark.pageIndex = pageIndex;
                // 跨行的命中會有多個 quad，逐一保留而不是併成一個大矩形：
                // 併起來會把行首到行尾之間沒被命中的內容一起塗掉。
                for (const domain::QuadPoint& quad : text::quadsForRange(*page, match.range)) {
                    mark.areas.push_back(padded(quad.boundingBox(), options.padding));
                }
                if (mark.areas.empty()) continue;

                mark.fillColor = options.fillColor;
                mark.author = options.author;
                mark.subject = options.subject;
                mark.note = options.note;
                mark.overlayText = options.overlayText;
                result.marks.add(std::move(mark));
            }
        });
        extractor.waitForIdle();
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::redaction
