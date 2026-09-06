#pragma once


// 圖磚定義。PRD-VIEW-001 嚴禁整頁光柵化——頁面一律切成固定尺寸圖磚，
// 只渲染與可視區相交者，外圍預取一圈。

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>

#include "geometry.h"

namespace alioth::domain {

inline constexpr std::int32_t kTileSize = 512;

// 縮放係數的圖磚鍵。
//
// 鍵是「倍率乘以 1000 取整」，也就是千分之一倍的解析度。用整數而不是浮點，
// 是因為快取鍵必須能精確比較——浮點的 1.2999999 與 1.3 是兩個不同的鍵，
// 快取命中率會莫名其妙掉到零。
//
// 兩種鍵並存，對應兩個不同的階段：
//
//   coarseScaleKey：連續縮放**過程中**用。把倍率吸附到 1.1 的等比階梯上，
//     讓整段縮放共用同一批圖磚，不必每個中間倍率都重算一輪。畫面上會是
//     暫時的拉伸結果，這是可接受的過場。
//
//   exactScaleKey：縮放**停止後**用（防抖 80 毫秒）。以使用者實際要的倍率重算。
//     PRD §4.3 明令「禁止以拉伸結果作為最終畫面」——那正是這個鍵存在的唯一理由。
inline constexpr double kZoomLadderRatio = 1.1;

[[nodiscard]] inline std::int32_t exactScaleKey(double scale) noexcept {
    return static_cast<std::int32_t>(std::lround(scale * 1000.0));
}

[[nodiscard]] inline std::int32_t coarseScaleKey(double scale) noexcept {
    if (scale <= 0.0) return 0;
    const double rung = std::lround(std::log(scale) / std::log(kZoomLadderRatio));
    return exactScaleKey(std::pow(kZoomLadderRatio, rung));
}

[[nodiscard]] inline double scaleOfKey(std::int32_t key) noexcept {
    return static_cast<double>(key) / 1000.0;
}

struct TileKey {
    std::int32_t pageIndex{0};
    std::int32_t scaleKey{0};  // 見 exactScaleKey / coarseScaleKey
    std::int32_t column{0};
    std::int32_t row{0};
    Rotation rotation{Rotation::None};
    bool nightMode{false};

    friend constexpr bool operator==(const TileKey&, const TileKey&) = default;
};

// 圖磚在裝置空間中的像素矩形。
[[nodiscard]] inline RectI tileDeviceRect(const TileKey& key) noexcept {
    return RectI{key.column * kTileSize, key.row * kTileSize, kTileSize, kTileSize};
}

// 渲染優先權。可見圖磚一律優先於預取，預取優先於縮圖（PRD §4.2）。
enum class TaskPriority : std::uint8_t {
    Visible = 0,
    Prefetch = 1,
    Thumbnail = 2,
    Background = 3,
};

}  // namespace alioth::domain

template <>
struct std::hash<alioth::domain::TileKey> {
    std::size_t operator()(const alioth::domain::TileKey& k) const noexcept {
        std::size_t h = 1469598103934665603ULL;
        const auto mix = [&h](std::size_t v) {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(static_cast<std::size_t>(k.pageIndex));
        mix(static_cast<std::size_t>(k.scaleKey));
        mix(static_cast<std::size_t>(k.column));
        mix(static_cast<std::size_t>(k.row));
        mix(static_cast<std::size_t>(k.rotation));
        mix(static_cast<std::size_t>(k.nightMode));
        return h;
    }
};
