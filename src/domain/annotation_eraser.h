#pragma once

// Eraser：擦除手繪註解（PRD-ANN-020）。
//
// 只作用在 /Ink 幾何：沿著使用者拖曳的擦除路徑,把落在半徑內的原始筆畫點
// 移除。一條筆畫被從中間挖掉一段時會斷成兩條——不是把整條筆畫刪掉,那樣
// 「只碰到一小段就整條消失」不是使用者期待的橡皮擦行為。
//
// 純函數,輸入輸出都是 domain::InkGeometry,不觸碰 domain/annotation.h
// 既有定義,也不知道自己身處哪一則 Annotation——呼叫端決定擦完後筆畫是否
// 已經空了、該不該把整則註解刪除。

#include <cmath>
#include <cstddef>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::domain {

struct EraseOptions {
    // 擦除半徑（頁面座標,點為單位）。任何與擦除路徑上「任一取樣點」距離
    // 小於等於這個半徑的筆畫點都會被移除。
    double radius{5.0};

    // 一段筆畫被擦到只剩下這麼少的點時整段丟棄,而不是保留一兩個孤點——
    // 那種殘留在畫面上只會是視覺雜訊,量測不出任何形狀。
    int minRemainingPoints{2};
};

namespace detail {

[[nodiscard]] inline double distanceSquared(const PointF& a, const PointF& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// 點 p 到路徑 eraserPath 上任一取樣點的最短距離是否落在半徑內。
// eraserPath 本身在呼叫端已經是一串取樣點（拖曳事件產生),因此不需要
// 額外的線段最近點計算——取樣密度夠高時逐點比對已經足夠精確,而且
// 不必為了求線段最近點另外引入分支。
[[nodiscard]] inline bool withinRadius(const PointF& p, const std::vector<PointF>& eraserPath,
                                       double radius) noexcept {
    if (!(radius > 0.0)) return false;
    const double r2 = radius * radius;
    for (const PointF& e : eraserPath) {
        if (distanceSquared(p, e) <= r2) return true;
    }
    return false;
}

}  // namespace detail

// 用擦除路徑處理一則 InkGeometry,回傳擦除後的結果（可能筆畫數量增加,
// 因為中間被挖空的筆畫會斷成兩段；也可能變空,代表整則註解該被刪除）。
[[nodiscard]] inline InkGeometry eraseFromInk(const InkGeometry& ink,
                                              const std::vector<PointF>& eraserPath,
                                              const EraseOptions& options = {}) {
    InkGeometry result{};
    if (eraserPath.empty()) return ink;

    for (const std::vector<PointF>& stroke : ink.strokes) {
        std::vector<PointF> current;
        current.reserve(stroke.size());
        for (const PointF& p : stroke) {
            if (detail::withinRadius(p, eraserPath, options.radius)) {
                // 命中:把目前累積的一段收尾,開始新的一段。
                if (static_cast<int>(current.size()) >= options.minRemainingPoints) {
                    result.strokes.push_back(std::move(current));
                }
                current.clear();
                continue;
            }
            current.push_back(p);
        }
        if (static_cast<int>(current.size()) >= options.minRemainingPoints) {
            result.strokes.push_back(std::move(current));
        }
    }
    return result;
}

// 擦除後是否等同於「整則註解都被擦掉了」。
[[nodiscard]] inline bool isFullyErased(const InkGeometry& ink) noexcept {
    return ink.strokes.empty();
}

}  // namespace alioth::domain
