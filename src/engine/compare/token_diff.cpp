#include "engine/compare/token_diff.h"

#include <algorithm>
#include <utility>

namespace alioth::engine::compare {
namespace {

using domain::DiffKind;
using domain::EditSpan;
using domain::TextRange;

// 一段共同子序列。middle snake 分治的產物是一堆這種片段，
// 收齊之後排序再串成編輯腳本——比在遞迴中維持輸出順序簡單，
// 也讓遞迴可以改寫成迭代（不可信任輸入禁止深度未定的遞迴，SDD §7）。
struct MatchRun {
    std::int32_t oldBegin{0};
    std::int32_t newBegin{0};
    std::int32_t length{0};
};

struct Snake {
    std::int32_t xStart{0};
    std::int32_t yStart{0};
    std::int32_t xEnd{0};
    std::int32_t yEnd{0};
};

struct Problem {
    std::int32_t aLo{0};
    std::int32_t aHi{0};
    std::int32_t bLo{0};
    std::int32_t bHi{0};
};

class MyersEngine {
public:
    MyersEngine(std::span<const TokenId> a, std::span<const TokenId> b, std::uint64_t maxCost)
        : a_(a), b_(b), maxCost_(maxCost) {
        const std::size_t total = a.size() + b.size();
        // 對角線索引 k 的範圍是 [-maxD, maxD]，而讀取會碰到 k±1，
        // 所以偏移量要比 maxD 再多幾格。多留 4 格是為了讓 N+M 為奇數時也不必分兩種算法。
        offset_ = static_cast<std::int32_t>(total / 2) + 4;
        const std::size_t width = static_cast<std::size_t>(offset_) * 2 + 2;
        forward_.assign(width, 0);
        backward_.assign(width, 0);
    }

    void run() {
        std::vector<Problem> stack;
        stack.push_back(Problem{0, static_cast<std::int32_t>(a_.size()), 0,
                                static_cast<std::int32_t>(b_.size())});

        // 分治的展開次數上限。正常情形遠低於此；設上限是為了讓任何實作瑕疵
        // 變成「降級」而不是「掛住」——不可信任輸入不允許無界迴圈。
        const std::uint64_t maxPops = 4 * (a_.size() + b_.size()) + 64;
        std::uint64_t pops = 0;

        while (!stack.empty()) {
            if (++pops > maxPops) {
                degraded_ = true;
                return;
            }
            Problem p = stack.back();
            stack.pop_back();

            while (p.aLo < p.aHi && p.bLo < p.bHi && a_[p.aLo] == b_[p.bLo]) {
                matches_.push_back(MatchRun{p.aLo, p.bLo, 1});
                ++p.aLo;
                ++p.bLo;
            }
            while (p.aLo < p.aHi && p.bLo < p.bHi && a_[p.aHi - 1] == b_[p.bHi - 1]) {
                --p.aHi;
                --p.bHi;
                matches_.push_back(MatchRun{p.aHi, p.bHi, 1});
            }
            // 有一側空了就沒有共同段可找，缺口由最後的串接階段補成插入或刪除。
            if (p.aLo >= p.aHi || p.bLo >= p.bHi) continue;

            Snake snake{};
            if (!findMiddleSnake(p, snake)) {
                degraded_ = true;
                continue;
            }
            // 分割必須讓兩個子問題都嚴格變小，否則就是無限迴圈。
            // 這個條件在演算法上恆成立，但輸入不可信任時寧可多一道檢查。
            const bool leftIsParent = snake.xStart >= p.aHi && snake.yStart >= p.bHi;
            const bool rightIsParent = snake.xEnd <= p.aLo && snake.yEnd <= p.bLo;
            if (leftIsParent && rightIsParent) {
                degraded_ = true;
                continue;
            }
            if (snake.xEnd > snake.xStart) {
                matches_.push_back(MatchRun{snake.xStart, snake.yStart, snake.xEnd - snake.xStart});
            }
            stack.push_back(Problem{snake.xEnd, p.aHi, snake.yEnd, p.bHi});
            stack.push_back(Problem{p.aLo, snake.xStart, p.bLo, snake.yStart});
        }
    }

    [[nodiscard]] bool degraded() const noexcept { return degraded_; }
    [[nodiscard]] std::uint64_t cost() const noexcept { return cost_; }
    [[nodiscard]] std::vector<MatchRun>& matches() noexcept { return matches_; }

private:
    // Myers §4b 的 middle snake：前向與後向同時推進，第一次交會處就是最佳路徑的中點。
    // 只保留兩條 V 陣列，因此記憶體與差異量無關，恆為 O(N+M)。
    bool findMiddleSnake(const Problem& p, Snake& out) {
        const std::int32_t n = p.aHi - p.aLo;
        const std::int32_t m = p.bHi - p.bLo;
        const std::int32_t delta = n - m;
        const bool odd = (delta & 1) != 0;
        const std::int32_t maxD = (n + m + 1) / 2 + 1;

        forward_[static_cast<std::size_t>(offset_ + 1)] = 0;
        backward_[static_cast<std::size_t>(offset_ + 1)] = 0;

        for (std::int32_t d = 0; d <= maxD; ++d) {
            cost_ += static_cast<std::uint64_t>(d) * 2 + 2;
            if (cost_ > maxCost_) return false;

            for (std::int32_t k = -d; k <= d; k += 2) {
                const std::size_t kc = static_cast<std::size_t>(offset_ + k);
                std::int32_t x = 0;
                if (k == -d || (k != d && forward_[kc - 1] < forward_[kc + 1])) {
                    x = forward_[kc + 1];
                } else {
                    x = forward_[kc - 1] + 1;
                }
                std::int32_t y = x - k;
                const std::int32_t x0 = x;
                const std::int32_t y0 = y;
                while (x < n && y < m && a_[p.aLo + x] == b_[p.bLo + y]) {
                    ++x;
                    ++y;
                }
                forward_[kc] = x;
                ++cost_;
                // 前向對角線 k 對應到後向的 delta-k；範圍限制保證讀到的是
                // 上一輪後向剛寫過的格子，而不是上一次呼叫殘留的值。
                if (odd && k >= delta - (d - 1) && k <= delta + (d - 1)) {
                    if (x + backward_[static_cast<std::size_t>(offset_ + delta - k)] >= n) {
                        out = Snake{p.aLo + x0, p.bLo + y0, p.aLo + x, p.bLo + y};
                        return true;
                    }
                }
            }

            for (std::int32_t k = -d; k <= d; k += 2) {
                const std::size_t kc = static_cast<std::size_t>(offset_ + k);
                std::int32_t x = 0;
                if (k == -d || (k != d && backward_[kc - 1] < backward_[kc + 1])) {
                    x = backward_[kc + 1];
                } else {
                    x = backward_[kc - 1] + 1;
                }
                std::int32_t y = x - k;
                const std::int32_t x0 = x;
                const std::int32_t y0 = y;
                while (x < n && y < m && a_[p.aHi - 1 - x] == b_[p.bHi - 1 - y]) {
                    ++x;
                    ++y;
                }
                backward_[kc] = x;
                ++cost_;
                if (!odd && (delta - k) >= -d && (delta - k) <= d) {
                    if (x + forward_[static_cast<std::size_t>(offset_ + delta - k)] >= n) {
                        out = Snake{p.aLo + n - x, p.bLo + m - y, p.aLo + n - x0, p.bLo + m - y0};
                        return true;
                    }
                }
            }
        }
        return false;
    }

    std::span<const TokenId> a_;
    std::span<const TokenId> b_;
    std::vector<std::int32_t> forward_;
    std::vector<std::int32_t> backward_;
    std::vector<MatchRun> matches_;
    std::int32_t offset_{0};
    std::uint64_t cost_{0};
    std::uint64_t maxCost_{0};
    bool degraded_{false};
};

void appendChange(std::vector<EditSpan>& spans, std::int32_t oldFrom, std::int32_t oldTo,
                  std::int32_t newFrom, std::int32_t newTo) {
    if (oldFrom >= oldTo && newFrom >= newTo) return;
    EditSpan span{};
    span.oldSpan = TextRange{oldFrom, oldTo};
    span.newSpan = TextRange{newFrom, newTo};
    if (oldFrom < oldTo && newFrom < newTo) {
        span.kind = DiffKind::Replace;
    } else if (oldFrom < oldTo) {
        span.kind = DiffKind::Delete;
    } else {
        span.kind = DiffKind::Insert;
    }
    spans.push_back(span);
}

std::vector<EditSpan> buildSpans(std::vector<MatchRun>& matches, std::int32_t oldCount,
                                 std::int32_t newCount) {
    std::sort(matches.begin(), matches.end(), [](const MatchRun& l, const MatchRun& r) {
        return l.oldBegin < r.oldBegin;
    });

    std::vector<EditSpan> spans;
    std::int32_t ai = 0;
    std::int32_t bi = 0;
    for (const MatchRun& m : matches) {
        if (m.length <= 0) continue;
        // 分治產出的片段可能相鄰，合併後 Equal 段才會是完整的一句而不是一堆單字。
        if (!spans.empty() && spans.back().kind == DiffKind::Equal &&
            spans.back().oldSpan.end == m.oldBegin && spans.back().newSpan.end == m.newBegin) {
            spans.back().oldSpan.end += m.length;
            spans.back().newSpan.end += m.length;
            ai = spans.back().oldSpan.end;
            bi = spans.back().newSpan.end;
            continue;
        }
        appendChange(spans, ai, m.oldBegin, bi, m.newBegin);
        EditSpan equal{};
        equal.kind = DiffKind::Equal;
        equal.oldSpan = TextRange{m.oldBegin, m.oldBegin + m.length};
        equal.newSpan = TextRange{m.newBegin, m.newBegin + m.length};
        spans.push_back(equal);
        ai = equal.oldSpan.end;
        bi = equal.newSpan.end;
    }
    appendChange(spans, ai, oldCount, bi, newCount);
    return spans;
}

}  // namespace

TokenId TokenTable::intern(std::string_view text) {
    const auto it = ids_.find(std::string(text));
    if (it != ids_.end()) return it->second;
    const TokenId id = static_cast<TokenId>(ids_.size());
    ids_.emplace(std::string(text), id);
    return id;
}

TokenId TokenTable::lookup(std::string_view text) const {
    const auto it = ids_.find(std::string(text));
    return it == ids_.end() ? kUnknown : it->second;
}

const char* describe(DiffStatus status) noexcept {
    switch (status) {
        case DiffStatus::Ok: return "ok";
        case DiffStatus::Degraded: return "degraded";
        case DiffStatus::InputTooLarge: return "input too large";
    }
    return "unknown";
}

TokenDiff diffTokens(std::span<const TokenId> oldTokens, std::span<const TokenId> newTokens,
                     const DiffLimits& limits) {
    TokenDiff result{};
    if (oldTokens.size() > limits.maxTokensPerSide || newTokens.size() > limits.maxTokensPerSide) {
        result.status = DiffStatus::InputTooLarge;
        return result;
    }

    const std::int32_t oldCount = static_cast<std::int32_t>(oldTokens.size());
    const std::int32_t newCount = static_cast<std::int32_t>(newTokens.size());

    // 空輸入不必進演算法，而且 offset 的計算在兩側皆空時沒有意義。
    if (oldCount == 0 || newCount == 0) {
        appendChange(result.spans, 0, oldCount, 0, newCount);
        return result;
    }

    MyersEngine engine(oldTokens, newTokens, limits.maxCost);
    engine.run();
    result.spans = buildSpans(engine.matches(), oldCount, newCount);
    result.cost = engine.cost();
    result.status = engine.degraded() ? DiffStatus::Degraded : DiffStatus::Ok;
    return result;
}

}  // namespace alioth::engine::compare
