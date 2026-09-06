#pragma once

// 尺規／參考線（PRD-VIEW-015）與格線貼齊（PRD-VIEW-016）的純邏輯核心。
//
// 一切座標都是「頁面空間」（見 geometry.h 的座標系說明：原點左下、Y 向上、
// 單位點）。從尺規拖出參考線、在檢視上顯示格線，都是呈現層把這裡算出的頁面
// 座標經 PageTransform 轉成裝置座標之後才畫出來的事——貼齊判斷本身完全不碰
// 螢幕座標，這樣才能在任何縮放與旋轉下維持座標往返自洽（測試見
// tests/viewaids/test_guides.cpp）。

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "domain/geometry.h"

namespace alioth::domain {

enum class GuideOrientation : std::uint8_t {
    Horizontal,  // 一條水平線，以 y（頁面座標）定位，由「上方水平尺規」拖出
    Vertical,    // 一條垂直線，以 x 定位，由「左側垂直尺規」拖出
};

struct GuideLine {
    GuideOrientation orientation{GuideOrientation::Horizontal};
    double positionPt{0.0};  // 頁面空間座標：Horizontal 用 y，Vertical 用 x
    bool locked{false};
};

// 一頁的參考線集合。參考線是檢視層概念，不寫進 PDF 內容串流。
class GuideSet {
public:
    std::int32_t add(GuideLine line) {
        lines_.push_back(line);
        return static_cast<std::int32_t>(lines_.size()) - 1;
    }

    void remove(std::int32_t index) {
        if (index < 0 || static_cast<std::size_t>(index) >= lines_.size()) return;
        lines_.erase(lines_.begin() + index);
    }

    // 拖曳移動：鎖定的參考線忽略請求。
    void move(std::int32_t index, double newPositionPt) {
        if (index < 0 || static_cast<std::size_t>(index) >= lines_.size()) return;
        if (lines_[static_cast<std::size_t>(index)].locked) return;
        lines_[static_cast<std::size_t>(index)].positionPt = newPositionPt;
    }

    void setLocked(std::int32_t index, bool locked) {
        if (index < 0 || static_cast<std::size_t>(index) >= lines_.size()) return;
        lines_[static_cast<std::size_t>(index)].locked = locked;
    }

    void clear() { lines_.clear(); }

    [[nodiscard]] const std::vector<GuideLine>& lines() const noexcept { return lines_; }

private:
    std::vector<GuideLine> lines_;
};

struct GridSettings {
    bool enabled{false};
    double spacingPt{28.35};  // 預設約 10mm（1pt = 1/72in）
};

// 貼齊結果：每一軸各自回報是否有貼齊、貼到了什麼（格線／參考線／物件邊），
// 用於呈現層畫出貼齊提示線；沒有任何來源命中時該軸維持原值、snapped 為 false。
enum class SnapSource : std::uint8_t { None, Grid, Guide, ObjectEdge };

struct AxisSnapResult {
    double value{0.0};
    bool snapped{false};
    SnapSource source{SnapSource::None};
};

struct SnapResult {
    AxisSnapResult x;
    AxisSnapResult y;

    [[nodiscard]] PointF point() const noexcept { return {x.value, y.value}; }
};

// 對單一數值找最近的格線刻度，命中容差內才回報貼齊。
[[nodiscard]] inline AxisSnapResult snapToGrid(double value, const GridSettings& grid,
                                                double tolerancePt) {
    AxisSnapResult result{value, false, SnapSource::None};
    if (!grid.enabled || grid.spacingPt <= 0.0) return result;
    const double nearest = std::round(value / grid.spacingPt) * grid.spacingPt;
    if (std::abs(nearest - value) <= tolerancePt) {
        result.value = nearest;
        result.snapped = true;
        result.source = SnapSource::Grid;
    }
    return result;
}

// 對單一數值找最近的同方向參考線。
[[nodiscard]] inline AxisSnapResult snapToGuides(double value, GuideOrientation axisOrientation,
                                                  const std::vector<GuideLine>& guides,
                                                  double tolerancePt) {
    AxisSnapResult result{value, false, SnapSource::None};
    double bestDelta = tolerancePt;
    for (const auto& guide : guides) {
        if (guide.orientation != axisOrientation) continue;
        const double delta = std::abs(guide.positionPt - value);
        if (delta <= bestDelta) {
            bestDelta = delta;
            result.value = guide.positionPt;
            result.snapped = true;
            result.source = SnapSource::Guide;
        }
    }
    return result;
}

// 對單一數值找最近的候選邊（既有註解/物件的邊界值，已由呼叫端投影到單一軸）。
[[nodiscard]] inline AxisSnapResult snapToEdges(double value, const std::vector<double>& edges,
                                                 double tolerancePt) {
    AxisSnapResult result{value, false, SnapSource::None};
    double bestDelta = tolerancePt;
    for (const double edge : edges) {
        const double delta = std::abs(edge - value);
        if (delta <= bestDelta) {
            bestDelta = delta;
            result.value = edge;
            result.snapped = true;
            result.source = SnapSource::ObjectEdge;
        }
    }
    return result;
}

// 綜合貼齊：優先順序參考線 > 物件邊 > 格線（參考線是使用者主動放的，意圖最明確；
// 物件邊次之；格線最後，因為它永遠存在、優先權太高會讓貼參考線變得很難）。
[[nodiscard]] inline AxisSnapResult snapAxis(double value, GuideOrientation axisOrientation,
                                              const GuideSet& guides, const GridSettings& grid,
                                              const std::vector<double>& objectEdges,
                                              double tolerancePt) {
    if (auto g = snapToGuides(value, axisOrientation, guides.lines(), tolerancePt); g.snapped) {
        return g;
    }
    if (auto e = snapToEdges(value, objectEdges, tolerancePt); e.snapped) return e;
    return snapToGrid(value, grid, tolerancePt);
}

[[nodiscard]] inline SnapResult snapPoint(const PointF& pagePoint, const GuideSet& guides,
                                          const GridSettings& grid,
                                          const std::vector<double>& verticalEdges,
                                          const std::vector<double>& horizontalEdges,
                                          double tolerancePt) {
    SnapResult result;
    result.x = snapAxis(pagePoint.x, GuideOrientation::Vertical, guides, grid, verticalEdges,
                        tolerancePt);
    result.y = snapAxis(pagePoint.y, GuideOrientation::Horizontal, guides, grid, horizontalEdges,
                        tolerancePt);
    return result;
}

}  // namespace alioth::domain
