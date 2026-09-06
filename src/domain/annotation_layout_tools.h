#pragma once

// 多選操作的純幾何核心（PRD-ANN-011）:對齊、鍵盤微調、跨頁/跨文件貼上位移。
//
// 這裡只算「位移量」與「新矩形」,不碰事件處理、剪貼簿格式或選取狀態——
// 那些是呈現層與 app 層的事,依 WP28 的範圍界線本檔不接。呼叫端把目前選取
// 的一批 RectF（通常取自 Annotation::rect 或量測幾何的外接框）丟進來,
// 拿到的是同樣數量的新 RectF,逐一寫回對應註解。
//
// 鎖定的註解（AnnotationFlag::Locked）不參與位移計算的「移動」部分:
// alignSelection／nudgeSelection 對鎖定項目一律回傳原始矩形不變,
// 但仍然占用該索引位置,呼叫端不必自己過濾,索引與輸入一一對應。

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::domain {

enum class AlignEdge : std::uint8_t {
    Left,
    Right,
    Top,
    Bottom,
    CenterHorizontal,  // 沿垂直中線對齊（矩形的水平中心對齊到同一條 X）
    CenterVertical,    // 沿水平中線對齊（矩形的垂直中心對齊到同一條 Y）
};

enum class DistributeAxis : std::uint8_t {
    Horizontal,
    Vertical,
};

namespace detail {

[[nodiscard]] inline RectF translated(const RectF& r, double dx, double dy) noexcept {
    return RectF{r.left + dx, r.bottom + dy, r.right + dx, r.top + dy};
}

}  // namespace detail

// 一個帶鎖定狀態的選取項,對齊/分散/微調都以這個為輸入單位。
struct SelectableRect {
    RectF rect{};
    bool locked{false};
};

// 依 edge 對齊一批矩形。基準線取自「未鎖定項目」的外接框；若全部都鎖定則
// 無事可做,原樣回傳——不能用鎖定項目自己的邊當基準,那樣鎖定項目其實在
// "被動" 決定對齊線,語意上仍是它被納入了操作。
[[nodiscard]] inline std::vector<RectF> alignSelection(const std::vector<SelectableRect>& items,
                                                        AlignEdge edge) {
    std::vector<RectF> out;
    out.reserve(items.size());
    for (const SelectableRect& item : items) out.push_back(item.rect);
    if (items.size() < 2) return out;

    bool haveReference = false;
    RectF reference{};
    for (const SelectableRect& item : items) {
        if (item.locked) continue;
        if (!haveReference) {
            reference = item.rect;
            haveReference = true;
        } else {
            reference = reference.united(item.rect);
        }
    }
    if (!haveReference) return out;

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].locked) continue;
        const RectF& r = items[i].rect;
        double dx = 0.0;
        double dy = 0.0;
        switch (edge) {
            case AlignEdge::Left:              dx = reference.left - r.left; break;
            case AlignEdge::Right:             dx = reference.right - r.right; break;
            case AlignEdge::Top:               dy = reference.top - r.top; break;
            case AlignEdge::Bottom:            dy = reference.bottom - r.bottom; break;
            case AlignEdge::CenterHorizontal: {
                const double refCx = (reference.left + reference.right) * 0.5;
                const double cx = (r.left + r.right) * 0.5;
                dx = refCx - cx;
                break;
            }
            case AlignEdge::CenterVertical: {
                const double refCy = (reference.bottom + reference.top) * 0.5;
                const double cy = (r.bottom + r.top) * 0.5;
                dy = refCy - cy;
                break;
            }
        }
        out[i] = detail::translated(r, dx, dy);
    }
    return out;
}

// 沿指定軸等間距分散(至少三個未鎖定項目才有意義;不足時原樣回傳)。
// 排序依當前位置的中心點,首尾兩個維持原位,只調整中間的間距。
[[nodiscard]] inline std::vector<RectF> distributeSelection(const std::vector<SelectableRect>& items,
                                                             DistributeAxis axis) {
    std::vector<RectF> out;
    out.reserve(items.size());
    for (const SelectableRect& item : items) out.push_back(item.rect);

    std::vector<std::size_t> movable;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].locked) movable.push_back(i);
    }
    if (movable.size() < 3) return out;

    const auto centerOf = [&](std::size_t i) {
        const RectF& r = items[i].rect;
        return axis == DistributeAxis::Horizontal ? (r.left + r.right) * 0.5
                                                   : (r.bottom + r.top) * 0.5;
    };

    std::sort(movable.begin(), movable.end(),
             [&](std::size_t a, std::size_t b) { return centerOf(a) < centerOf(b); });

    const double firstCenter = centerOf(movable.front());
    const double lastCenter = centerOf(movable.back());
    const double span = lastCenter - firstCenter;
    const double step = span / static_cast<double>(movable.size() - 1);

    for (std::size_t rank = 1; rank + 1 < movable.size(); ++rank) {
        const std::size_t idx = movable[rank];
        const double targetCenter = firstCenter + step * static_cast<double>(rank);
        const double delta = targetCenter - centerOf(idx);
        out[idx] = axis == DistributeAxis::Horizontal ? detail::translated(items[idx].rect, delta, 0.0)
                                                       : detail::translated(items[idx].rect, 0.0, delta);
    }
    return out;
}

// 鍵盤微調:每個未鎖定項目平移 (dx, dy)。stepPt 是呼叫端已經換算好的
// 頁面座標位移量(例如一般箭頭鍵 1pt、Shift+箭頭 10pt),本函式不做倍率判斷。
[[nodiscard]] inline std::vector<RectF> nudgeSelection(const std::vector<SelectableRect>& items,
                                                        double dx, double dy) {
    std::vector<RectF> out;
    out.reserve(items.size());
    for (const SelectableRect& item : items) {
        out.push_back(item.locked ? item.rect : detail::translated(item.rect, dx, dy));
    }
    return out;
}

// 跨頁/跨文件貼上位移:把來源頁的矩形換算到目的頁,保留相對於各自頁面
// 左上角的位置(貼到不同尺寸的頁面時,以左上為錨點是 Acrobat 的慣例,
// 貼到比原頁小的目的頁時內容仍然對齊得上,而不是憑空按頁面中心對齊)。
[[nodiscard]] inline RectF pasteOffsetForPage(const RectF& sourceRect, const SizeF& sourcePageSize,
                                              const SizeF& targetPageSize) noexcept {
    const double sourceTopOffset = sourcePageSize.height - sourceRect.top;
    const double targetTop = targetPageSize.height - sourceTopOffset;
    const double dy = targetTop - sourceRect.top;
    return RectF{sourceRect.left, sourceRect.bottom + dy, sourceRect.right, sourceRect.top + dy};
}

// 同頁貼上(例如 Ctrl+V 貼在原位置附近):固定位移量,避免貼上結果完全疊在
// 複製來源正上方而看不出是新的一份。
[[nodiscard]] inline RectF pasteOffsetSamePage(const RectF& sourceRect, double offset = 12.0) noexcept {
    return RectF{sourceRect.left + offset, sourceRect.bottom - offset, sourceRect.right + offset,
                 sourceRect.top - offset};
}

}  // namespace alioth::domain
