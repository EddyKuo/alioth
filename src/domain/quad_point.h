#pragma once

// QuadPoints（ISO 32000-2 §12.5.6.10）的唯一定義。
//
// 這個型別原本在註解模型與文字層各有一份，欄位名還不一樣。兩者都對，但同一個
// 概念存在兩個定義違反 IL-3，而且只要有檔案同時 include 兩邊就會是重定義錯誤——
// 文字選取產生 quad 再交給螢光筆註解，正是那個必然會發生的檔案。
//
// 角序沿用 PDF 規格的 x1 y1 … x4 y4：左上、右上、左下、右下。注意這個順序不是
// 繞行順序（左下與右下是反的），照著它繞會畫出蝴蝶結；要繞行請用 corners()。

#include <algorithm>
#include <array>
#include <vector>

#include "geometry.h"

namespace alioth::domain {

struct QuadPoint {
    PointF upperLeft{};
    PointF upperRight{};
    PointF lowerLeft{};
    PointF lowerRight{};

    friend constexpr bool operator==(const QuadPoint&, const QuadPoint&) = default;

    // 由軸對齊矩形建 quad。文字擷取層給的字元框就是這種矩形。
    [[nodiscard]] static constexpr QuadPoint fromRect(const RectF& r) noexcept {
        return QuadPoint{{r.left, r.top}, {r.right, r.top}, {r.left, r.bottom}, {r.right, r.bottom}};
    }

    [[nodiscard]] RectF boundingBox() const noexcept {
        const double left = std::min({upperLeft.x, upperRight.x, lowerLeft.x, lowerRight.x});
        const double right = std::max({upperLeft.x, upperRight.x, lowerLeft.x, lowerRight.x});
        const double bottom = std::min({upperLeft.y, upperRight.y, lowerLeft.y, lowerRight.y});
        const double top = std::max({upperLeft.y, upperRight.y, lowerLeft.y, lowerRight.y});
        return RectF{left, bottom, right, top};
    }

    // 寫進 /QuadPoints 陣列的順序。序列化一律走這裡，不要在呼叫端自己排——
    // 順序寫錯時 Acrobat 仍可能畫得出來，但 macOS 預覽會是空的或錯位。
    [[nodiscard]] std::vector<double> toArray() const {
        return {upperLeft.x, upperLeft.y, upperRight.x,  upperRight.y,
                lowerLeft.x, lowerLeft.y, lowerRight.x,  lowerRight.y};
    }

    // 繞行順序：左上 → 右上 → 右下 → 左下。畫路徑時用這個，不要直接照欄位順序。
    [[nodiscard]] std::array<PointF, 4> corners() const noexcept {
        return {upperLeft, upperRight, lowerRight, lowerLeft};
    }
};

}  // namespace alioth::domain
