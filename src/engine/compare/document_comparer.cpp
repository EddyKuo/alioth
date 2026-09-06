#include "engine/compare/document_comparer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

#include "engine/objects/pdf_source_document.h"
#include "engine/text/text_extractor.h"

namespace alioth::engine::compare {
namespace {

using domain::DiffKind;
using domain::PageMatchKind;
using domain::TextRange;

[[nodiscard]] std::string joinTokens(const std::vector<Token>& tokens, TextRange span) {
    std::string out;
    for (std::int32_t i = span.start; i < span.end; ++i) {
        if (i < 0 || i >= static_cast<std::int32_t>(tokens.size())) break;
        if (!out.empty()) out.push_back(' ');
        out += tokens[static_cast<std::size_t>(i)].text;
    }
    return out;
}

// token 索引區間 → 頁內字元索引區間。
//
// 空區間（純插入時的另一側）回傳一個零長度的位置而不是空值：
// 並排標示需要在另一邊畫一個插入點的游標，沒有位置就只能畫在頁首。
[[nodiscard]] TextRange charRangeOf(const std::vector<Token>& tokens, TextRange span) {
    const std::int32_t count = static_cast<std::int32_t>(tokens.size());
    if (span.start >= span.end) {
        std::int32_t anchor = 0;
        if (span.start > 0 && span.start <= count) {
            anchor = tokens[static_cast<std::size_t>(span.start) - 1].charRange.end;
        } else if (count > 0) {
            anchor = tokens.front().charRange.start;
        }
        return TextRange{anchor, anchor};
    }
    const std::int32_t lo = std::clamp(span.start, 0, count - 1);
    const std::int32_t hi = std::clamp(span.end, 1, count);
    return TextRange{tokens[static_cast<std::size_t>(lo)].charRange.start,
                     tokens[static_cast<std::size_t>(hi) - 1].charRange.end};
}

void countRegion(domain::PageDiffSummary& summary, DiffKind kind) {
    switch (kind) {
        case DiffKind::Insert: ++summary.insertions; break;
        case DiffKind::Delete: ++summary.deletions; break;
        case DiffKind::Replace: ++summary.replacements; break;
        case DiffKind::Equal: break;
    }
}

[[nodiscard]] std::map<std::int32_t, const PageTokens*> indexByPage(
    std::span<const PageTokens> pages) {
    std::map<std::int32_t, const PageTokens*> out;
    for (const PageTokens& page : pages) out.emplace(page.pageIndex, &page);
    return out;
}

// 依序擷取一份文件的所有頁面文字。
//
// 每次只讓一個 TextExtractor 存在，且在回頭讀結果之前先 waitForIdle：
// tokenize 是在文字執行緒上執行的，共用的 TokenTable 沒有鎖，
// 兩份文件同時擷取會同時寫進同一張表。這是不加並行的第二個理由。
struct ExtractOutcome {
    domain::DocumentError error{domain::DocumentError::None};
    std::vector<PageTokens> pages;
};

[[nodiscard]] ExtractOutcome extractPages(const std::string& path, const std::string& password,
                                          TokenTable& table, bool ignoreCase,
                                          std::size_t maxPages) {
    ExtractOutcome outcome{};
    text::TextExtractor extractor;

    domain::DocumentError openError = domain::DocumentError::Unknown;
    extractor.open(path, password, [&openError](domain::DocumentError e) { openError = e; });
    extractor.waitForIdle();
    if (openError != domain::DocumentError::None) {
        outcome.error = openError;
        return outcome;
    }

    const std::int32_t pageCount = extractor.pageCount();
    if (pageCount < 0 || static_cast<std::size_t>(pageCount) > maxPages) {
        outcome.error = domain::DocumentError::UnsupportedFeature;
        extractor.close();
        extractor.waitForIdle();
        return outcome;
    }

    outcome.pages.resize(static_cast<std::size_t>(pageCount));
    for (std::int32_t i = 0; i < pageCount; ++i) {
        outcome.pages[static_cast<std::size_t>(i)].pageIndex = i;
        extractor.withTextPage(i, [&outcome, &table, ignoreCase, i](const text::TextPage* page) {
            if (page == nullptr) return;
            outcome.pages[static_cast<std::size_t>(i)] =
                tokenizePage(page->layer(), table, ignoreCase);
        });
    }
    extractor.waitForIdle();
    extractor.close();
    extractor.waitForIdle();
    return outcome;
}

[[nodiscard]] std::string readAllBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// 頁面的呈現尺寸：/MediaBox 套上 /Rotate。
//
// 走物件層而不是再開一個 PDFium 把手，是為了不在同一份檔案上疊第二個把手
// （見標頭的執行緒說明）。/CropBox 與 /UserUnit 刻意忽略：重複頁偵測比的是
// 頁面的實體尺寸，裁切框是檢視設定而不是頁面本身。
[[nodiscard]] std::vector<domain::SizeF> readPageSizes(const std::string& path,
                                                       std::size_t expectedPages) {
    std::vector<domain::SizeF> sizes(expectedPages, domain::SizeF{});
    objects::PdfSourceDocument source;
    if (source.open(readAllBytes(path)) != objects::SourceStatus::Ok) return sizes;

    const std::vector<objects::PdfRef>& pages = source.pages();
    const std::size_t count = std::min(expectedPages, pages.size());
    for (std::size_t i = 0; i < count; ++i) {
        const objects::PdfObject box =
            source.resolve(source.inheritedPageAttribute(pages[i], "MediaBox"));
        const objects::PdfArray* array = box.asArray();
        if (array == nullptr || array->size() < 4) continue;
        const double x0 = source.resolve((*array)[0]).asNumber();
        const double y0 = source.resolve((*array)[1]).asNumber();
        const double x1 = source.resolve((*array)[2]).asNumber();
        const double y1 = source.resolve((*array)[3]).asNumber();
        double width = std::fabs(x1 - x0);
        double height = std::fabs(y1 - y0);

        const objects::PdfObject rotate =
            source.resolve(source.inheritedPageAttribute(pages[i], "Rotate"));
        std::int64_t degrees = rotate.isNumber() ? rotate.asInteger() : 0;
        degrees = ((degrees % 360) + 360) % 360;
        if (degrees == 90 || degrees == 270) std::swap(width, height);

        sizes[i] = domain::SizeF{width, height};
    }
    return sizes;
}

}  // namespace

domain::DocumentDiff comparePageTexts(std::span<const PageTokens> oldPages,
                                      std::span<const PageTokens> newPages,
                                      const CompareOptions& options) {
    domain::DocumentDiff diff;

    const AlignResult aligned = alignPages(oldPages, newPages, options.align, options.diff);
    if (aligned.rejected) {
        // 拒絕不是「沒有差異」。呼叫端必須看得出來，否則會把它當成兩份相同的文件。
        diff.setDegraded(true);
        return diff;
    }
    if (aligned.degraded) diff.setDegraded(true);

    const std::map<std::int32_t, const PageTokens*> oldIndex = indexByPage(oldPages);
    const std::map<std::int32_t, const PageTokens*> newIndex = indexByPage(newPages);

    std::vector<domain::PageDiffSummary> summaries;
    summaries.reserve(aligned.alignments.size());

    for (const domain::PageAlignment& alignment : aligned.alignments) {
        diff.addAlignment(alignment);

        domain::PageDiffSummary summary{};
        summary.kind = alignment.kind;
        summary.oldPage = alignment.oldPage;
        summary.newPage = alignment.newPage;
        summary.similarity = alignment.similarity;

        const auto oldIt = oldIndex.find(alignment.oldPage);
        const auto newIt = newIndex.find(alignment.newPage);
        const PageTokens* oldPage = oldIt == oldIndex.end() ? nullptr : oldIt->second;
        const PageTokens* newPage = newIt == newIndex.end() ? nullptr : newIt->second;

        if (alignment.kind == PageMatchKind::Deleted && oldPage != nullptr) {
            if (!oldPage->tokens.empty()) {
                domain::TextDiffRegion region{};
                region.kind = DiffKind::Delete;
                region.oldPage = alignment.oldPage;
                region.newPage = domain::kNoPage;
                region.oldRange = charRangeOf(
                    oldPage->tokens,
                    TextRange{0, static_cast<std::int32_t>(oldPage->tokens.size())});
                region.oldText = joinTokens(
                    oldPage->tokens,
                    TextRange{0, static_cast<std::int32_t>(oldPage->tokens.size())});
                countRegion(summary, DiffKind::Delete);
                diff.addRegion(std::move(region));
            }
            summaries.push_back(summary);
            continue;
        }
        if (alignment.kind == PageMatchKind::Inserted && newPage != nullptr) {
            if (!newPage->tokens.empty()) {
                domain::TextDiffRegion region{};
                region.kind = DiffKind::Insert;
                region.oldPage = domain::kNoPage;
                region.newPage = alignment.newPage;
                region.newRange = charRangeOf(
                    newPage->tokens,
                    TextRange{0, static_cast<std::int32_t>(newPage->tokens.size())});
                region.newText = joinTokens(
                    newPage->tokens,
                    TextRange{0, static_cast<std::int32_t>(newPage->tokens.size())});
                countRegion(summary, DiffKind::Insert);
                diff.addRegion(std::move(region));
            }
            summaries.push_back(summary);
            continue;
        }
        if (oldPage == nullptr || newPage == nullptr) {
            summaries.push_back(summary);
            continue;
        }

        const std::vector<TokenId> oldIds = oldPage->ids();
        const std::vector<TokenId> newIds = newPage->ids();
        const TokenDiff pageDiff = diffTokens(oldIds, newIds, options.diff);
        if (pageDiff.status == DiffStatus::Degraded ||
            pageDiff.status == DiffStatus::InputTooLarge) {
            diff.setDegraded(true);
        }
        for (const domain::EditSpan& span : pageDiff.spans) {
            if (span.kind == DiffKind::Equal) continue;
            domain::TextDiffRegion region{};
            region.kind = span.kind;
            region.oldPage = alignment.oldPage;
            region.newPage = alignment.newPage;
            region.oldRange = charRangeOf(oldPage->tokens, span.oldSpan);
            region.newRange = charRangeOf(newPage->tokens, span.newSpan);
            region.oldText = joinTokens(oldPage->tokens, span.oldSpan);
            region.newText = joinTokens(newPage->tokens, span.newSpan);
            countRegion(summary, span.kind);
            diff.addRegion(std::move(region));
        }
        summaries.push_back(summary);
    }

    diff.setSummaries(std::move(summaries));
    return diff;
}

CompareResult compareDocuments(const std::string& oldPath, const std::string& newPath,
                               const CompareOptions& options, const std::string& oldPassword,
                               const std::string& newPassword) {
    CompareResult result{};

    // 同一張 TokenTable：兩份文件的 token id 必須在同一個號碼空間裡，
    // 否則同一個詞在兩邊是不同的 id，比出來會是「整份都改了」。
    TokenTable table;

    ExtractOutcome oldOutcome =
        extractPages(oldPath, oldPassword, table, options.ignoreCase, options.align.maxPages);
    if (oldOutcome.error != domain::DocumentError::None) {
        result.error = oldOutcome.error;
        result.oldDocumentFailed = true;
        return result;
    }

    ExtractOutcome newOutcome =
        extractPages(newPath, newPassword, table, options.ignoreCase, options.align.maxPages);
    if (newOutcome.error != domain::DocumentError::None) {
        result.error = newOutcome.error;
        return result;
    }

    result.diff = comparePageTexts(oldOutcome.pages, newOutcome.pages, options);
    if (result.diff.alignments().empty() && !(oldOutcome.pages.empty() && newOutcome.pages.empty())) {
        result.error = domain::DocumentError::UnsupportedFeature;
    }
    return result;
}

DuplicateScanResult scanDuplicatePages(const std::string& path, const DuplicateOptions& options,
                                       const std::string& password) {
    DuplicateScanResult result{};

    text::TextExtractor extractor;
    domain::DocumentError openError = domain::DocumentError::Unknown;
    extractor.open(path, password, [&openError](domain::DocumentError e) { openError = e; });
    extractor.waitForIdle();
    if (openError != domain::DocumentError::None) {
        result.error = openError;
        return result;
    }

    const std::int32_t pageCount = extractor.pageCount();
    std::vector<PageFingerprintInput> inputs(static_cast<std::size_t>(std::max(pageCount, 0)));
    for (std::int32_t i = 0; i < pageCount; ++i) {
        inputs[static_cast<std::size_t>(i)].pageIndex = i;
        extractor.withTextPage(i, [&inputs, i](const text::TextPage* page) {
            if (page == nullptr) return;
            const domain::PageTextLayer& layer = page->layer();
            inputs[static_cast<std::size_t>(i)].text = layer.text(layer.fullRange());
        });
    }
    extractor.waitForIdle();
    extractor.close();
    extractor.waitForIdle();

    const std::vector<domain::SizeF> sizes = readPageSizes(path, inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i].sizePt = sizes[i];

    result.groups = findDuplicatePages(inputs, options);
    return result;
}

}  // namespace alioth::engine::compare
