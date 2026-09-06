#include "page_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace alioth::domain {

void PageLayout::setPageSizes(std::vector<SizeF> sizesPt) {
    sizesPt_ = std::move(sizesPt);
    if (currentPage_ >= static_cast<std::int32_t>(sizesPt_.size())) {
        currentPage_ = 0;
    }
}

SizeF PageLayout::deviceSizeOf(std::int32_t pageIndex) const {
    if (pageIndex < 0 || pageIndex >= static_cast<std::int32_t>(sizesPt_.size())) {
        return {};
    }
    const PageTransform transform(sizesPt_[static_cast<std::size_t>(pageIndex)], scale_, rotation_);
    return transform.deviceSize();
}

// 把頁面分組成「列」。單頁模式一列一頁且只有當前頁；雙頁模式兩頁一列，
// 封面獨立時第一列只有一頁。
std::vector<std::vector<std::int32_t>> PageLayout::buildRows() const {
    const auto total = static_cast<std::int32_t>(sizesPt_.size());
    std::vector<std::vector<std::int32_t>> rows;
    if (total == 0) return rows;

    if (options_.mode == LayoutMode::Horizontal) {
        // 一整列裝下所有頁面。列的概念在這裡退化，但重用同一條擺放路徑
        // 比另寫一份省得多——兩份擺放程式碼遲早會在間隙或旋轉上分歧。
        std::vector<std::int32_t> row;
        row.reserve(static_cast<std::size_t>(total));
        for (std::int32_t i = 0; i < total; ++i) row.push_back(i);
        rows.push_back(std::move(row));
        return rows;
    }

    const bool twoUp = options_.mode == LayoutMode::TwoPage ||
                       options_.mode == LayoutMode::TwoPageContinuous;
    const bool continuous = options_.mode == LayoutMode::Continuous ||
                            options_.mode == LayoutMode::TwoPageContinuous;

    if (!continuous) {
        // 非連續模式只排出當前這一組，捲軸範圍才會等於單頁（或單組）高度。
        std::vector<std::int32_t> row;
        if (!twoUp) {
            row.push_back(std::clamp(currentPage_, 0, total - 1));
        } else {
            std::int32_t left = std::clamp(currentPage_, 0, total - 1);
            if (options_.coverPageSeparate) {
                // 封面獨立時，成對的是 (1,2)、(3,4)…，因此左頁必為奇數索引。
                if (left != 0) left = left % 2 == 0 ? left - 1 : left;
            } else {
                left = left - (left % 2);
            }
            row.push_back(left);
            if (!(options_.coverPageSeparate && left == 0) && left + 1 < total) {
                row.push_back(left + 1);
            }
        }
        rows.push_back(std::move(row));
        return rows;
    }

    if (!twoUp) {
        for (std::int32_t i = 0; i < total; ++i) {
            rows.push_back({i});
        }
        return rows;
    }

    std::int32_t index = 0;
    if (options_.coverPageSeparate) {
        rows.push_back({0});
        index = 1;
    }
    while (index < total) {
        std::vector<std::int32_t> row{index};
        if (index + 1 < total) row.push_back(index + 1);
        rows.push_back(std::move(row));
        index += 2;
    }
    return rows;
}

void PageLayout::update(double scale, Rotation rotation, const LayoutOptions& options,
                        std::int32_t viewportWidth) {
    scale_ = scale;
    rotation_ = rotation;
    options_ = options;
    placements_.clear();
    contentSize_ = {};

    const auto rows = buildRows();
    if (rows.empty()) return;

    const auto gap = static_cast<std::int32_t>(std::lround(options_.pageGapPx));

    // 先算出最寬的一列，所有列以它為基準置中，捲動時頁面才不會左右跳動。
    double widest = 0.0;
    for (const auto& row : rows) {
        double rowWidth = 0.0;
        for (std::size_t i = 0; i < row.size(); ++i) {
            rowWidth += deviceSizeOf(row[i]).width;
            if (i + 1 < row.size()) rowWidth += gap;
        }
        widest = std::max(widest, rowWidth);
    }

    const double canvasWidth = std::max(widest, static_cast<double>(viewportWidth));
    std::int32_t y = gap;

    for (const auto& row : rows) {
        double rowWidth = 0.0;
        double rowHeight = 0.0;
        for (std::size_t i = 0; i < row.size(); ++i) {
            const SizeF size = deviceSizeOf(row[i]);
            rowWidth += size.width;
            rowHeight = std::max(rowHeight, size.height);
            if (i + 1 < row.size()) rowWidth += gap;
        }

        auto x = static_cast<std::int32_t>(std::lround((canvasWidth - rowWidth) / 2.0));

        // 右至左：整列的視覺順序反過來，但頁碼順序不變（PRD-VIEW-012）。
        std::vector<std::int32_t> ordered = row;
        if (options_.rightToLeft) {
            std::reverse(ordered.begin(), ordered.end());
        }

        for (const std::int32_t pageIndex : ordered) {
            const SizeF size = deviceSizeOf(pageIndex);
            PagePlacement placement;
            placement.pageIndex = pageIndex;
            // 水平排列時各頁高度不一，靠上對齊會讓一列頁面看起來像參差的鋸齒。
            // 其他模式維持原本的靠上對齊，免得既有版面在這次改動下移位。
            std::int32_t pageY = y;
            if (options_.mode == LayoutMode::Horizontal) {
                pageY = y + static_cast<std::int32_t>(std::lround((rowHeight - size.height) / 2.0));
            }
            placement.rect = RectI{x, pageY, static_cast<std::int32_t>(std::lround(size.width)),
                                   static_cast<std::int32_t>(std::lround(size.height))};
            placements_.push_back(placement);
            x += placement.rect.width + gap;
        }

        y += static_cast<std::int32_t>(std::lround(rowHeight)) + gap;
    }

    // 排序成頁碼順序，讓 pageRect 與可視區判定不受右至左影響。
    std::sort(placements_.begin(), placements_.end(),
              [](const PagePlacement& a, const PagePlacement& b) {
                  return a.pageIndex < b.pageIndex;
              });

    contentSize_ = SizeF{canvasWidth, static_cast<double>(y)};
}

RectI PageLayout::pageRect(std::int32_t pageIndex) const {
    const auto it = std::lower_bound(placements_.begin(), placements_.end(), pageIndex,
                                     [](const PagePlacement& p, std::int32_t index) {
                                         return p.pageIndex < index;
                                     });
    if (it == placements_.end() || it->pageIndex != pageIndex) return {};
    return it->rect;
}

std::vector<PagePlacement> PageLayout::visiblePages(const RectI& viewport) const {
    std::vector<PagePlacement> visible;
    for (const PagePlacement& placement : placements_) {
        if (placement.rect.intersects(viewport)) {
            visible.push_back(placement);
        }
    }
    return visible;
}

std::int32_t PageLayout::pageAtViewportCenter(const RectI& viewport) const {
    if (placements_.empty()) return 0;
    const std::int32_t centerY = viewport.y + viewport.height / 2;

    // 中點可能落在頁間間隙上，此時取最接近的一頁而不是回報「沒有頁面」。
    std::int32_t best = placements_.front().pageIndex;
    std::int32_t bestDistance = std::numeric_limits<std::int32_t>::max();
    for (const PagePlacement& placement : placements_) {
        if (centerY >= placement.rect.y && centerY < placement.rect.bottom()) {
            return placement.pageIndex;
        }
        const std::int32_t distance =
            centerY < placement.rect.y ? placement.rect.y - centerY : centerY - placement.rect.bottom();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = placement.pageIndex;
        }
    }
    return best;
}

PointF PageLayout::toPageLocal(std::int32_t pageIndex, const PointF& documentPoint) const {
    const RectI rect = pageRect(pageIndex);
    return PointF{documentPoint.x - rect.x, documentPoint.y - rect.y};
}

}  // namespace alioth::domain
