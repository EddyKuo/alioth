#pragma once

// 領域層幾何型別。PDF 頁面座標系：單位為點（1/72 吋），原點在左下角，Y 軸向上。
// 螢幕座標系原點在左上角、Y 軸向下，兩者的轉換一律經由 PageTransform，
// 禁止在其他地方手寫 Y 軸翻轉——那是座標錯誤最常見的來源。

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace alioth::domain {

struct PointF {
    double x{0.0};
    double y{0.0};

    friend constexpr bool operator==(const PointF&, const PointF&) = default;
};

struct SizeF {
    double width{0.0};
    double height{0.0};

    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0.0 || height <= 0.0; }

    friend constexpr bool operator==(const SizeF&, const SizeF&) = default;
};

// 軸對齊矩形，以左下與右上兩點表示（與 PDF /Rect 慣例一致）。
struct RectF {
    double left{0.0};
    double bottom{0.0};
    double right{0.0};
    double top{0.0};

    [[nodiscard]] constexpr double width() const noexcept { return right - left; }
    [[nodiscard]] constexpr double height() const noexcept { return top - bottom; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width() <= 0.0 || height() <= 0.0; }
    [[nodiscard]] constexpr SizeF size() const noexcept { return {width(), height()}; }

    [[nodiscard]] constexpr bool contains(const PointF& p) const noexcept {
        return p.x >= left && p.x <= right && p.y >= bottom && p.y <= top;
    }

    [[nodiscard]] constexpr bool intersects(const RectF& o) const noexcept {
        return left < o.right && o.left < right && bottom < o.top && o.bottom < top;
    }

    [[nodiscard]] constexpr RectF intersected(const RectF& o) const noexcept {
        return RectF{std::max(left, o.left), std::max(bottom, o.bottom),
                     std::min(right, o.right), std::min(top, o.top)};
    }

    [[nodiscard]] constexpr RectF united(const RectF& o) const noexcept {
        if (isEmpty()) return o;
        if (o.isEmpty()) return *this;
        return RectF{std::min(left, o.left), std::min(bottom, o.bottom),
                     std::max(right, o.right), std::max(top, o.top)};
    }

    [[nodiscard]] static constexpr RectF fromSize(double w, double h) noexcept {
        return RectF{0.0, 0.0, w, h};
    }

    [[nodiscard]] RectF normalized() const noexcept {
        return RectF{std::min(left, right), std::min(bottom, top),
                     std::max(left, right), std::max(bottom, top)};
    }

    friend constexpr bool operator==(const RectF&, const RectF&) = default;
};

// 整數像素矩形，用於圖磚與可視區。原點左上、Y 向下。
struct RectI {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};

    [[nodiscard]] constexpr std::int32_t right() const noexcept { return x + width; }
    [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }

    [[nodiscard]] constexpr bool intersects(const RectI& o) const noexcept {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }

    friend constexpr bool operator==(const RectI&, const RectI&) = default;
};

// 檢視層頁面旋轉。不修改文件，只影響呈現（PRD-VIEW-005）。
enum class Rotation : std::uint8_t {
    None = 0,
    Cw90 = 1,
    Cw180 = 2,
    Cw270 = 3,
};

[[nodiscard]] constexpr int rotationDegrees(Rotation r) noexcept {
    return static_cast<int>(r) * 90;
}

[[nodiscard]] constexpr Rotation addRotation(Rotation a, Rotation b) noexcept {
    return static_cast<Rotation>((static_cast<int>(a) + static_cast<int>(b)) % 4);
}

[[nodiscard]] constexpr bool swapsAxes(Rotation r) noexcept {
    return r == Rotation::Cw90 || r == Rotation::Cw270;
}

// 頁面空間 → 裝置空間的仿射轉換。
//
// 這是全專案唯一該做 Y 軸翻轉與旋轉的地方。scale 已含 DPI 係數，
// 呼叫端不要再額外乘 devicePixelRatio。
class PageTransform {
public:
    PageTransform() = default;

    PageTransform(SizeF pageSizePt, double scale, Rotation rotation) noexcept
        : pageSize_(pageSizePt), scale_(scale), rotation_(rotation) {}

    [[nodiscard]] double scale() const noexcept { return scale_; }
    [[nodiscard]] Rotation rotation() const noexcept { return rotation_; }
    [[nodiscard]] SizeF pageSizePt() const noexcept { return pageSize_; }

    // 旋轉與縮放後的頁面像素尺寸。
    [[nodiscard]] SizeF deviceSize() const noexcept {
        const double w = pageSize_.width * scale_;
        const double h = pageSize_.height * scale_;
        return swapsAxes(rotation_) ? SizeF{h, w} : SizeF{w, h};
    }

    [[nodiscard]] PointF toDevice(const PointF& pagePt) const noexcept {
        const double sx = pagePt.x * scale_;
        // 頁面 Y 向上、裝置 Y 向下，先翻轉再旋轉。
        const double sy = (pageSize_.height - pagePt.y) * scale_;
        const SizeF dev = deviceSize();
        switch (rotation_) {
            case Rotation::None:  return {sx, sy};
            case Rotation::Cw90:  return {dev.width - sy, sx};
            case Rotation::Cw180: return {dev.width - sx, dev.height - sy};
            case Rotation::Cw270: return {sy, dev.height - sx};
        }
        return {sx, sy};
    }

    [[nodiscard]] PointF toPage(const PointF& devicePt) const noexcept {
        const SizeF dev = deviceSize();
        double sx = devicePt.x;
        double sy = devicePt.y;
        switch (rotation_) {
            case Rotation::None:  break;
            case Rotation::Cw90:  { const double t = sx; sx = sy; sy = dev.width - t; break; }
            case Rotation::Cw180: { sx = dev.width - sx; sy = dev.height - sy; break; }
            case Rotation::Cw270: { const double t = sx; sx = dev.height - sy; sy = t; break; }
        }
        return {sx / scale_, pageSize_.height - sy / scale_};
    }

    [[nodiscard]] RectF toDeviceRect(const RectF& pageRect) const noexcept {
        const PointF a = toDevice({pageRect.left, pageRect.bottom});
        const PointF b = toDevice({pageRect.right, pageRect.top});
        return RectF{std::min(a.x, b.x), std::min(a.y, b.y),
                     std::max(a.x, b.x), std::max(a.y, b.y)};
    }

private:
    SizeF pageSize_{};
    double scale_{1.0};
    Rotation rotation_{Rotation::None};
};

}  // namespace alioth::domain
