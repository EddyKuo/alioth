#pragma once

// 鉛筆手繪的平滑化（PRD-ANN-003）。
//
// 輸入是使用者原始的觸控/滑鼠取樣點（可能帶壓力值），輸出是平滑後的點序列，
// 用來降低取樣抖動造成的鋸齒。刻意不用參數化曲線重新取樣（例如轉成貝茲曲線
// 再等距取點）：那種作法會讓端點與轉角的位置系統性偏移，使用者畫的形狀會
// 「認不出來」。這裡改用移動平均，並且對每一點的位移量設硬性上限——
// 平滑後的每一點與原始點的距離不得超過 maxDeviation，這是本檔案存在的
// 唯一理由，也是測試要鎖住的不變量。
//
// 純函數、不依賴 Qt 或 PDFium，才能在領域層被密集測試（SDD §8）。
// 輸出仍是 std::vector<PointF>，經 domain::InkGeometry 走既有的外觀產生
// 與寫入通道；本檔不觸碰 domain/annotation.h。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "domain/geometry.h"

namespace alioth::domain {

// 一個原始取樣點：座標加上壓力（0–1，裝置不支援壓力時一律回報 1.0）。
// 壓力目前只用於決定「該不該平滑」與供上層決定筆寬，不改變座標本身——
// PDF 的 /Ink 只有單一線寬，壓力驅動的變寬效果留給未來的多筆畫模擬
// （已知限制，見本檔案 smoothStroke 的說明）。
struct PressurePoint {
    PointF position{};
    double pressure{1.0};

    friend constexpr bool operator==(const PressurePoint&, const PressurePoint&) = default;
};

struct SmoothingOptions {
    // 移動平均視窗半徑（單邊取樣點數）。1 代表用左右各一點加自己取平均，
    // 也就是三點平均；0 代表不平滑，直接回傳原始點。
    int windowRadius{1};

    // 平滑後的點與原始點的最大允許位移（頁面座標，點為單位）。任何一次平均
    // 若會讓位移超過這個值，改用「原始點與理想平均點之間、剛好落在上限距離處
    // 的插值點」，而不是整段跳過平滑——這樣既保住形狀輪廓,也保證測試可驗證
    // 的硬上限恆成立。
    double maxDeviation{2.0};
};

namespace detail {

[[nodiscard]] inline double distance(const PointF& a, const PointF& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// 把 candidate 拉回到 origin 為圓心、maxDeviation 為半徑的圓內；
// 已經在圓內則原樣回傳。
[[nodiscard]] inline PointF clampToDeviation(const PointF& origin, const PointF& candidate,
                                             double maxDeviation) noexcept {
    if (!(maxDeviation > 0.0)) return origin;
    const double d = distance(origin, candidate);
    if (d <= maxDeviation || d <= 0.0) return candidate;
    const double t = maxDeviation / d;
    return PointF{origin.x + (candidate.x - origin.x) * t, origin.y + (candidate.y - origin.y) * t};
}

}  // namespace detail

// 對一條筆畫的座標做移動平均平滑，壓力值原樣沿用（每點壓力不受平滑影響）。
//
// 兩端點（起筆、收筆）恆不平滑：使用者的落筆與提筆位置是意圖的一部分，
// 移動平均會把端點往內縮，讓筆畫看起來「變短」。
[[nodiscard]] inline std::vector<PressurePoint> smoothStroke(
    const std::vector<PressurePoint>& points, const SmoothingOptions& options = {}) {
    const std::size_t n = points.size();
    if (n < 3 || options.windowRadius <= 0) return points;

    std::vector<PressurePoint> out;
    out.reserve(n);
    out.push_back(points.front());

    for (std::size_t i = 1; i + 1 < n; ++i) {
        const std::size_t radius = static_cast<std::size_t>(options.windowRadius);
        const std::size_t lo = (i >= radius) ? (i - radius) : 0;
        const std::size_t hi = std::min(n - 1, i + radius);

        double sx = 0.0;
        double sy = 0.0;
        std::size_t count = 0;
        for (std::size_t k = lo; k <= hi; ++k) {
            sx += points[k].position.x;
            sy += points[k].position.y;
            ++count;
        }
        const PointF averaged{sx / static_cast<double>(count), sy / static_cast<double>(count)};
        const PointF clamped = detail::clampToDeviation(points[i].position, averaged, options.maxDeviation);
        out.push_back(PressurePoint{clamped, points[i].pressure});
    }

    out.push_back(points.back());
    return out;
}

// 便利版本：丟棄壓力,只回傳座標,供直接餵給 InkGeometry::strokes。
[[nodiscard]] inline std::vector<PointF> smoothStrokePositions(
    const std::vector<PointF>& points, const SmoothingOptions& options = {}) {
    std::vector<PressurePoint> withPressure;
    withPressure.reserve(points.size());
    for (const PointF& p : points) withPressure.push_back(PressurePoint{p, 1.0});
    const std::vector<PressurePoint> smoothed = smoothStroke(withPressure, options);
    std::vector<PointF> out;
    out.reserve(smoothed.size());
    for (const PressurePoint& p : smoothed) out.push_back(p.position);
    return out;
}

}  // namespace alioth::domain
