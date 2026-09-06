#include "engine/compare/page_align.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace alioth::engine::compare {
namespace {

using domain::DiffKind;
using domain::PageAlignment;
using domain::PageMatchKind;

[[nodiscard]] std::vector<TokenId> sortedIds(const PageTokens& page) {
    std::vector<TokenId> ids = page.ids();
    std::sort(ids.begin(), ids.end());
    return ids;
}

// 兩個已排序序列的多重集合交集大小。
[[nodiscard]] std::size_t commonCount(const std::vector<TokenId>& a,
                                      const std::vector<TokenId>& b) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    std::size_t common = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            ++i;
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            ++common;
            ++i;
            ++j;
        }
    }
    return common;
}

[[nodiscard]] double diceFromSorted(const std::vector<TokenId>& a, const std::vector<TokenId>& b,
                                    double threshold) noexcept {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    const double total = static_cast<double>(a.size() + b.size());
    // 長度差本身就足以否決時不必做交集：這是局部對齊裡最有效的剪枝，
    // 因為「頁數多但每頁長度差很多」正是動態規劃最貴的情形。
    const double upperBound = 2.0 * static_cast<double>(std::min(a.size(), b.size())) / total;
    if (upperBound < threshold) return upperBound;
    return 2.0 * static_cast<double>(commonCount(a, b)) / total;
}

// 局部對齊用的相似度快取。同一頁在動態規劃中會被問上百次，
// 每次重排序等於把 O(n·m) 變成 O(n·m·k log k)。
class SimilarityCache {
public:
    SimilarityCache(std::span<const PageTokens> oldPages, std::span<const PageTokens> newPages)
        : oldPages_(oldPages), newPages_(newPages) {}

    [[nodiscard]] double similarity(std::size_t oldIndex, std::size_t newIndex, double threshold) {
        return diceFromSorted(oldSorted(oldIndex), newSorted(newIndex), threshold);
    }

private:
    const std::vector<TokenId>& oldSorted(std::size_t index) {
        auto it = oldCache_.find(index);
        if (it == oldCache_.end()) {
            it = oldCache_.emplace(index, sortedIds(oldPages_[index])).first;
        }
        return it->second;
    }

    const std::vector<TokenId>& newSorted(std::size_t index) {
        auto it = newCache_.find(index);
        if (it == newCache_.end()) {
            it = newCache_.emplace(index, sortedIds(newPages_[index])).first;
        }
        return it->second;
    }

    std::span<const PageTokens> oldPages_;
    std::span<const PageTokens> newPages_;
    std::unordered_map<std::size_t, std::vector<TokenId>> oldCache_;
    std::unordered_map<std::size_t, std::vector<TokenId>> newCache_;
};

void appendMatched(AlignResult& result, const PageTokens& oldPage, const PageTokens& newPage,
                   double similarity) {
    PageAlignment alignment{};
    alignment.kind = PageMatchKind::Matched;
    alignment.oldPage = oldPage.pageIndex;
    alignment.newPage = newPage.pageIndex;
    alignment.similarity = similarity;
    result.alignments.push_back(alignment);
}

void appendDeleted(AlignResult& result, const PageTokens& oldPage) {
    PageAlignment alignment{};
    alignment.kind = PageMatchKind::Deleted;
    alignment.oldPage = oldPage.pageIndex;
    alignment.newPage = domain::kNoPage;
    result.alignments.push_back(alignment);
}

void appendInserted(AlignResult& result, const PageTokens& newPage) {
    PageAlignment alignment{};
    alignment.kind = PageMatchKind::Inserted;
    alignment.oldPage = domain::kNoPage;
    alignment.newPage = newPage.pageIndex;
    result.alignments.push_back(alignment);
}

// 超出比較預算時的後備：同一區塊內依序一對一配。
// 仍然檢查門檻，因此不會硬把兩頁不相干的內容說成同一頁的兩個版本。
void alignBlockPositional(std::span<const PageTokens> oldPages, std::span<const PageTokens> newPages,
                          std::size_t o0, std::size_t o1, std::size_t n0, std::size_t n1,
                          const AlignOptions& options, SimilarityCache& cache, AlignResult& result) {
    const std::size_t n = o1 - o0;
    const std::size_t m = n1 - n0;
    const std::size_t count = std::max(n, m);
    for (std::size_t k = 0; k < count; ++k) {
        const bool hasOld = k < n;
        const bool hasNew = k < m;
        if (hasOld && hasNew) {
            const double sim = cache.similarity(o0 + k, n0 + k, options.similarityThreshold);
            if (sim >= options.similarityThreshold) {
                appendMatched(result, oldPages[o0 + k], newPages[n0 + k], sim);
            } else {
                appendDeleted(result, oldPages[o0 + k]);
                appendInserted(result, newPages[n0 + k]);
            }
        } else if (hasOld) {
            appendDeleted(result, oldPages[o0 + k]);
        } else {
            appendInserted(result, newPages[n0 + k]);
        }
    }
}

void alignBlock(std::span<const PageTokens> oldPages, std::span<const PageTokens> newPages,
                std::size_t o0, std::size_t o1, std::size_t n0, std::size_t n1,
                const AlignOptions& options, SimilarityCache& cache, AlignResult& result) {
    const std::size_t n = o1 - o0;
    const std::size_t m = n1 - n0;
    if (n == 0 && m == 0) return;
    if (n == 0) {
        for (std::size_t j = n0; j < n1; ++j) appendInserted(result, newPages[j]);
        return;
    }
    if (m == 0) {
        for (std::size_t i = o0; i < o1; ++i) appendDeleted(result, oldPages[i]);
        return;
    }
    if (n * m > options.maxSimilarityComparisons) {
        result.degraded = true;
        alignBlockPositional(oldPages, newPages, o0, o1, n0, n1, options, cache, result);
        return;
    }

    // 最大化「配成對的相似度總和」的序列比對。跳過一頁不扣分，
    // 因為插入與刪除本身不是錯誤，我們只要盡量多配對真正相似的頁。
    const std::size_t stride = m + 1;
    std::vector<double> score((n + 1) * stride, 0.0);
    std::vector<std::uint8_t> back((n + 1) * stride, 0);
    constexpr std::uint8_t kDiagonal = 0;
    constexpr std::uint8_t kUp = 1;    // 消耗一頁舊的（刪除）
    constexpr std::uint8_t kLeft = 2;  // 消耗一頁新的（插入）

    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            const double up = score[(i - 1) * stride + j];
            const double left = score[i * stride + (j - 1)];
            double best = up;
            std::uint8_t choice = kUp;
            if (left > best) {
                best = left;
                choice = kLeft;
            }
            const double sim = cache.similarity(o0 + i - 1, n0 + j - 1, options.similarityThreshold);
            if (sim >= options.similarityThreshold) {
                const double diagonal = score[(i - 1) * stride + (j - 1)] + sim;
                // 相等時偏好對角：配成對的資訊量比跳過多。
                if (diagonal >= best) {
                    best = diagonal;
                    choice = kDiagonal;
                }
            }
            score[i * stride + j] = best;
            back[i * stride + j] = choice;
        }
    }

    std::vector<PageAlignment> reversed;
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0) {
        const std::uint8_t choice = back[i * stride + j];
        if (choice == kDiagonal) {
            const double sim = cache.similarity(o0 + i - 1, n0 + j - 1, options.similarityThreshold);
            PageAlignment alignment{};
            alignment.kind = PageMatchKind::Matched;
            alignment.oldPage = oldPages[o0 + i - 1].pageIndex;
            alignment.newPage = newPages[n0 + j - 1].pageIndex;
            alignment.similarity = sim;
            reversed.push_back(alignment);
            --i;
            --j;
        } else if (choice == kUp) {
            PageAlignment alignment{};
            alignment.kind = PageMatchKind::Deleted;
            alignment.oldPage = oldPages[o0 + i - 1].pageIndex;
            reversed.push_back(alignment);
            --i;
        } else {
            PageAlignment alignment{};
            alignment.kind = PageMatchKind::Inserted;
            alignment.newPage = newPages[n0 + j - 1].pageIndex;
            reversed.push_back(alignment);
            --j;
        }
    }
    while (i > 0) {
        PageAlignment alignment{};
        alignment.kind = PageMatchKind::Deleted;
        alignment.oldPage = oldPages[o0 + i - 1].pageIndex;
        reversed.push_back(alignment);
        --i;
    }
    while (j > 0) {
        PageAlignment alignment{};
        alignment.kind = PageMatchKind::Inserted;
        alignment.newPage = newPages[n0 + j - 1].pageIndex;
        reversed.push_back(alignment);
        --j;
    }

    result.alignments.insert(result.alignments.end(), reversed.rbegin(), reversed.rend());
}

}  // namespace

double pageSimilarity(const PageTokens& a, const PageTokens& b) {
    // 公開版本不帶門檻，因此不啟用剪枝：回傳值必須是真正的係數。
    const std::vector<TokenId> left = sortedIds(a);
    const std::vector<TokenId> right = sortedIds(b);
    if (left.empty() && right.empty()) return 1.0;
    if (left.empty() || right.empty()) return 0.0;
    return 2.0 * static_cast<double>(commonCount(left, right)) /
           static_cast<double>(left.size() + right.size());
}

AlignResult alignPages(std::span<const PageTokens> oldPages, std::span<const PageTokens> newPages,
                       const AlignOptions& options, const DiffLimits& diffLimits) {
    AlignResult result{};
    if (oldPages.size() > options.maxPages || newPages.size() > options.maxPages) {
        result.rejected = true;
        return result;
    }

    // 階段一：以整頁雜湊當 token 找出完全相同的頁面。
    // 雜湊碰撞會讓兩頁不同的內容被當成錨點，所以命中後仍逐 token 驗證。
    std::unordered_map<std::uint64_t, TokenId> hashIds;
    const auto idOf = [&hashIds](std::uint64_t hash) {
        const auto it = hashIds.find(hash);
        if (it != hashIds.end()) return it->second;
        const TokenId id = static_cast<TokenId>(hashIds.size());
        hashIds.emplace(hash, id);
        return id;
    };

    std::vector<TokenId> oldHashIds;
    std::vector<TokenId> newHashIds;
    oldHashIds.reserve(oldPages.size());
    newHashIds.reserve(newPages.size());
    for (const PageTokens& page : oldPages) oldHashIds.push_back(idOf(page.contentHash));
    for (const PageTokens& page : newPages) newHashIds.push_back(idOf(page.contentHash));

    const TokenDiff pageDiff = diffTokens(oldHashIds, newHashIds, diffLimits);
    if (!pageDiff.ok()) {
        result.rejected = true;
        return result;
    }
    if (pageDiff.status == DiffStatus::Degraded) result.degraded = true;

    SimilarityCache cache(oldPages, newPages);

    for (const domain::EditSpan& span : pageDiff.spans) {
        if (span.kind == DiffKind::Equal) {
            for (std::int32_t k = 0; k < span.oldSpan.count(); ++k) {
                const std::size_t oi = static_cast<std::size_t>(span.oldSpan.start + k);
                const std::size_t ni = static_cast<std::size_t>(span.newSpan.start + k);
                const double sim = pageSimilarity(oldPages[oi], newPages[ni]);
                appendMatched(result, oldPages[oi], newPages[ni], sim);
            }
            continue;
        }
        alignBlock(oldPages, newPages, static_cast<std::size_t>(span.oldSpan.start),
                   static_cast<std::size_t>(span.oldSpan.end),
                   static_cast<std::size_t>(span.newSpan.start),
                   static_cast<std::size_t>(span.newSpan.end), options, cache, result);
    }

    return result;
}

}  // namespace alioth::engine::compare
