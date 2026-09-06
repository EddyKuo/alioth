#pragma once

// 壓力感測筆寬（PRD-ANN-003 的後半）。
//
// PDF 的 /Ink 只有一個 /BS /W——整則註解共用一個線寬，規格裡沒有「每一點各自
// 粗細」的表達方式。要讓壓力真的影響筆畫粗細，唯一符合規格、而且在所有檢視器
// 都畫得出來的作法是：把一條筆畫依壓力切成數段，每一段用自己的線寬寫成
// **一則獨立的 /Ink 註解**。
//
// 這個檔案負責那個切法。三個刻意的決定：
//
//   1. **分檔而不是連續。** 壓力是連續值，但每個不同的寬度就是一則新註解；
//      逐點分段會讓一條 300 點的筆畫變成 300 則註解，檔案膨脹、註解清單爆掉、
//      Acrobat 開檔變慢。分成少數幾檔（預設 4）在視覺上已經看得出「用力比較粗」，
//      而註解數量維持在個位數。
//
//   2. **相鄰段共用交界點。** 段與段之間若各自從下一點開始，交界處會出現
//      一個線寬的缺口——筆畫看起來是斷的。每一段的起點一律是前一段的終點。
//
//   3. **壓力全程相同時退回單一筆畫。** 沒有壓力裝置的滑鼠一律回報 1.0，
//      這種情況必須產生與原本完全一樣的單一 /Ink，不能因為「支援壓力」
//      就讓每個用滑鼠的使用者都多出好幾則註解。
//
// 純函數、不依賴 Qt 或 PDFium。

#include <algorithm>
#include <cstddef>
#include <vector>

#include "domain/geometry.h"
#include "domain/ink_smoothing.h"

namespace alioth::domain {

struct PressureWidthOptions {
    // 壓力 0 與 1 對應的線寬（點）。兩者相等代表關閉壓力感測。
    double minWidth{0.6};
    double maxWidth{3.0};
    // 分幾檔。1 代表不分段（整條用平均壓力的寬度）。
    int bandCount{4};
};

// 同一個線寬底下的一批筆畫。一個 layer 對應一則要寫出的 /Ink 註解。
struct InkWidthLayer {
    double width{1.0};
    std::vector<std::vector<PointF>> strokes{};
};

namespace detail {

// 壓力 → 檔位。壓力值來自裝置，超出 0–1 是常見的（部分驅動回報 1.0 以上），
// 夾住而不是信任它。
[[nodiscard]] inline int bandOf(double pressure, int bandCount) noexcept {
    if (bandCount <= 1) return 0;
    const double clamped = std::clamp(pressure, 0.0, 1.0);
    const int band = static_cast<int>(clamped * static_cast<double>(bandCount));
    return std::min(band, bandCount - 1);
}

[[nodiscard]] inline double widthOfBand(int band, const PressureWidthOptions& options) noexcept {
    if (options.bandCount <= 1) return (options.minWidth + options.maxWidth) * 0.5;
    // 取每一檔的中點，而不是下緣：用下緣的話最重的那一檔永遠達不到 maxWidth。
    const double t = (static_cast<double>(band) + 0.5) / static_cast<double>(options.bandCount);
    return options.minWidth + (options.maxWidth - options.minWidth) * t;
}

}  // namespace detail

// 把一批（已平滑的）壓力筆畫依壓力切成數個線寬層。
//
// 回傳的層依線寬由細到粗排序，方便呼叫端穩定地產生註解；同一層裡的筆畫
// 維持原本的先後順序。空輸入回傳空結果——沒有筆畫時不該產生一則空註解。
[[nodiscard]] inline std::vector<InkWidthLayer> splitByPressure(
    const std::vector<std::vector<PressurePoint>>& strokes,
    const PressureWidthOptions& options = {}) {
    std::vector<InkWidthLayer> layers;
    const int bandCount = std::max(1, options.bandCount);

    // band → layers 裡的索引。用固定大小的表而不是 map：檔數是個位數，
    // 而且要保證輸出順序只由檔位決定，不受哪一條筆畫先出現影響。
    std::vector<int> layerOf(static_cast<std::size_t>(bandCount), -1);

    const auto appendSegment = [&](int band, std::vector<PointF> points) {
        if (points.size() < 2) return;  // 單點畫不出線，寫出去只是一則看不見的註解
        int& index = layerOf[static_cast<std::size_t>(band)];
        if (index < 0) {
            index = static_cast<int>(layers.size());
            PressureWidthOptions effective = options;
            effective.bandCount = bandCount;
            layers.push_back(InkWidthLayer{detail::widthOfBand(band, effective), {}});
        }
        layers[static_cast<std::size_t>(index)].strokes.push_back(std::move(points));
    };

    for (const std::vector<PressurePoint>& stroke : strokes) {
        if (stroke.size() < 2) continue;

        int currentBand = detail::bandOf(stroke.front().pressure, bandCount);
        std::vector<PointF> segment{stroke.front().position};
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            const int band = detail::bandOf(stroke[i].pressure, bandCount);
            if (band != currentBand) {
                // 交界點同時屬於兩段：前一段以它結束，下一段以它開始。
                // 少了這一步，兩段之間會出現一個線寬的缺口。
                segment.push_back(stroke[i].position);
                appendSegment(currentBand, std::move(segment));
                segment = std::vector<PointF>{stroke[i].position};
                currentBand = band;
                continue;
            }
            segment.push_back(stroke[i].position);
        }
        appendSegment(currentBand, std::move(segment));
    }

    std::sort(layers.begin(), layers.end(),
              [](const InkWidthLayer& a, const InkWidthLayer& b) { return a.width < b.width; });
    return layers;
}

}  // namespace alioth::domain
