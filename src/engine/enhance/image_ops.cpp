#include "engine/enhance/image_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace alioth::engine::enhance {
namespace {

using domain::enhance::DeskewResult;
using domain::enhance::DeskewSettings;
using domain::enhance::EnhanceSettings;

constexpr double kDegToRad = std::numbers::pi / 180.0;

// BT.601 亮度權重，以定點整數計算：浮點在這個迴圈裡沒有精度需求，
// 但有數量級的成本差異。
[[nodiscard]] inline std::uint8_t luma(std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept {
    const std::uint32_t value = (static_cast<std::uint32_t>(r) * 299u +
                                 static_cast<std::uint32_t>(g) * 587u +
                                 static_cast<std::uint32_t>(b) * 114u) / 1000u;
    return static_cast<std::uint8_t>(value > 255u ? 255u : value);
}

// 墨水像素的座標。投影剖面要對每個候選角度重掃一次，
// 若每次都走完整張圖，成本是「像素數 × 角度數」；
// 只留下墨水像素之後，文字頁通常只剩下 5–10%。
struct InkPixels {
    std::vector<std::int32_t> xs;
    std::vector<std::int32_t> ys;
    std::int32_t width{0};
    std::int32_t height{0};
};

[[nodiscard]] double projectionScore(const InkPixels& ink, double angleDeg, std::int32_t binOffset,
                                     std::vector<std::uint32_t>& profile) {
    std::fill(profile.begin(), profile.end(), 0u);
    const double radians = angleDeg * kDegToRad;
    const double s = std::sin(radians);
    const double c = std::cos(radians);
    const auto binCount = static_cast<std::int32_t>(profile.size());

    for (std::size_t i = 0; i < ink.xs.size(); ++i) {
        const double value = static_cast<double>(ink.xs[i]) * s + static_cast<double>(ink.ys[i]) * c;
        auto bin = static_cast<std::int32_t>(std::lround(value)) + binOffset;
        if (bin < 0) bin = 0;
        if (bin >= binCount) bin = binCount - 1;
        ++profile[static_cast<std::size_t>(bin)];
    }

    // 分數為剖面的平方和。墨水總量與角度無關，因此平方和只會在
    // 「墨水集中在少數幾條帶上」時變大，那正是文字行對齊的定義。
    double score = 0.0;
    for (const std::uint32_t value : profile) {
        const double v = static_cast<double>(value);
        score += v * v;
    }
    return score;
}

}  // namespace

GrayImage toGrayscale(const PixelBuffer& source) {
    GrayImage out;
    if (source.isNull()) return out;
    out.width = source.width();
    out.height = source.height();
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));

    const std::uint8_t* base = source.data();
    for (std::int32_t y = 0; y < out.height; ++y) {
        const std::uint8_t* src = base + source.stride() * static_cast<std::size_t>(y);
        std::uint8_t* dst = out.row(y);
        for (std::int32_t x = 0; x < out.width; ++x) {
            dst[x] = luma(src[x * 4 + 0], src[x * 4 + 1], src[x * 4 + 2]);
        }
    }
    return out;
}

GrayImage downscaleToMaxEdge(const GrayImage& source, std::int32_t maxEdge) {
    if (source.isNull() || maxEdge <= 0) return source;
    const std::int32_t longest = std::max(source.width, source.height);
    if (longest <= maxEdge) return source;

    // 整數倍率的盒式平均。非整數倍率需要加權，收益是次像素級的平滑，
    // 但去斜只看行結構，付那個成本沒有回報。
    const std::int32_t factor = (longest + maxEdge - 1) / maxEdge;
    GrayImage out;
    out.width = std::max(1, source.width / factor);
    out.height = std::max(1, source.height / factor);
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));

    for (std::int32_t y = 0; y < out.height; ++y) {
        std::uint8_t* dst = out.row(y);
        for (std::int32_t x = 0; x < out.width; ++x) {
            std::uint32_t sum = 0;
            std::uint32_t count = 0;
            for (std::int32_t dy = 0; dy < factor; ++dy) {
                const std::int32_t sy = y * factor + dy;
                if (sy >= source.height) break;
                for (std::int32_t dx = 0; dx < factor; ++dx) {
                    const std::int32_t sx = x * factor + dx;
                    if (sx >= source.width) break;
                    sum += source.at(sx, sy);
                    ++count;
                }
            }
            dst[x] = count == 0 ? 255 : static_cast<std::uint8_t>(sum / count);
        }
    }
    return out;
}

std::array<std::uint64_t, 256> grayHistogram(const GrayImage& image) {
    std::array<std::uint64_t, 256> histogram{};
    for (const std::uint8_t value : image.pixels) {
        ++histogram[value];
    }
    return histogram;
}

PixelBuffer clonePixels(const PixelBuffer& source) {
    if (source.isNull()) return PixelBuffer{};
    PixelBuffer out(source.width(), source.height());
    const std::size_t bytes = std::min(source.stride(), out.stride());
    for (std::int32_t y = 0; y < source.height(); ++y) {
        std::memcpy(out.scanline(y), source.data() + source.stride() * static_cast<std::size_t>(y),
                    bytes);
    }
    return out;
}

PixelBuffer makeBuffer(std::int32_t width, std::int32_t height, std::uint8_t gray) {
    PixelBuffer out(width, height);
    if (out.isNull()) return out;
    for (std::int32_t y = 0; y < height; ++y) {
        std::uint8_t* row = out.scanline(y);
        for (std::int32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = gray;
            row[x * 4 + 1] = gray;
            row[x * 4 + 2] = gray;
            row[x * 4 + 3] = 255;
        }
    }
    return out;
}

DeskewResult detectSkew(const GrayImage& image, const DeskewSettings& settings) {
    DeskewResult result;
    if (image.isNull() || !domain::enhance::settingsValid(settings)) {
        result.note = "影像為空或參數不合法";
        return result;
    }

    const GrayImage analysis = downscaleToMaxEdge(image, settings.analysisMaxEdge);
    const std::uint8_t threshold = domain::enhance::otsuThreshold(grayHistogram(analysis));

    InkPixels ink;
    ink.width = analysis.width;
    ink.height = analysis.height;
    for (std::int32_t y = 0; y < analysis.height; ++y) {
        for (std::int32_t x = 0; x < analysis.width; ++x) {
            if (analysis.at(x, y) < threshold) {
                ink.xs.push_back(x);
                ink.ys.push_back(y);
            }
        }
    }

    const auto totalPixels =
        static_cast<double>(analysis.width) * static_cast<double>(analysis.height);
    const double inkRatio = static_cast<double>(ink.xs.size()) / totalPixels;

    // 兩端都要擋：墨水太少代表頁面幾乎空白（只有頁碼的頁），
    // 墨水太多代表整頁是照片或反白版面。兩種情況的投影剖面都沒有行結構，
    // 硬算出來的最佳角度是雜訊，套用下去就是把好好的掃描件轉歪。
    if (inkRatio < 0.0005) {
        result.note = "墨水像素過少，無法判定傾斜";
        return result;
    }
    if (inkRatio > 0.9) {
        result.note = "墨水像素過多（可能整頁為影像），無法判定傾斜";
        return result;
    }

    const double maxRadians = settings.maxAngleDeg * kDegToRad;
    const auto binOffset = static_cast<std::int32_t>(
        std::ceil(static_cast<double>(analysis.width) * std::sin(maxRadians))) + 2;
    // 所有角度共用同一個分箱數，分數才可以直接互相比較；
    // 每個角度各自算範圍的話，分箱數不同會讓平方和天生偏向某些角度。
    const auto binCount = static_cast<std::size_t>(analysis.height + 2 * binOffset + 2);
    std::vector<std::uint32_t> profile(binCount, 0u);

    const double zeroScore = projectionScore(ink, 0.0, binOffset, profile);
    if (zeroScore <= 0.0) {
        result.note = "投影剖面為空";
        return result;
    }

    double bestAngle = 0.0;
    double bestScore = zeroScore;
    for (double angle = -settings.maxAngleDeg; angle <= settings.maxAngleDeg + 1e-9;
         angle += settings.coarseStepDeg) {
        const double score = projectionScore(ink, angle, binOffset, profile);
        if (score > bestScore) {
            bestScore = score;
            bestAngle = angle;
        }
    }

    const double fineLow = std::max(-settings.maxAngleDeg, bestAngle - settings.coarseStepDeg);
    const double fineHigh = std::min(settings.maxAngleDeg, bestAngle + settings.coarseStepDeg);
    for (double angle = fineLow; angle <= fineHigh + 1e-9; angle += settings.fineStepDeg) {
        const double score = projectionScore(ink, angle, binOffset, profile);
        if (score > bestScore) {
            bestScore = score;
            bestAngle = angle;
        }
    }

    result.angleDeg = bestAngle;
    result.scoreGain = (bestScore - zeroScore) / zeroScore;

    if (std::abs(bestAngle) < settings.minApplyAngleDeg) {
        result.note = "傾斜量低於門檻，視為水平";
        return result;
    }
    if (result.scoreGain < settings.minScoreGain) {
        // 分數曲線太平：可能是照片、地圖或沒有行結構的版面。
        // 這裡回報「沒有傾斜」而不是回報最佳角度，是為了保護正常的掃描件。
        result.note = "分數提升不足，可能沒有可辨識的行結構";
        return result;
    }

    result.detected = true;
    return result;
}

DeskewResult detectSkew(const PixelBuffer& image, const DeskewSettings& settings) {
    return detectSkew(toGrayscale(image), settings);
}

PixelBuffer rotate(const PixelBuffer& source, double degrees, std::uint8_t background) {
    if (source.isNull()) return PixelBuffer{};
    if (std::abs(degrees) < 1e-9) return clonePixels(source);

    const std::int32_t width = source.width();
    const std::int32_t height = source.height();
    PixelBuffer out = makeBuffer(width, height, background);

    const double radians = degrees * kDegToRad;
    const double s = std::sin(radians);
    const double c = std::cos(radians);
    const double cx = (static_cast<double>(width) - 1.0) / 2.0;
    const double cy = (static_cast<double>(height) - 1.0) / 2.0;

    const std::uint8_t* base = source.data();
    const std::size_t stride = source.stride();

    for (std::int32_t y = 0; y < height; ++y) {
        std::uint8_t* dst = out.scanline(y);
        const double v = static_cast<double>(y) - cy;
        for (std::int32_t x = 0; x < width; ++x) {
            const double u = static_cast<double>(x) - cx;
            // 反向映射取樣。正向映射（把來源點灑到目的地）會留下沒被寫到的
            // 空洞，而空洞在文字上表現為斷筆，看起來像掃描機的問題。
            const double sx = cx + (u * c - v * s);
            const double sy = cy + (u * s + v * c);
            const auto x0 = static_cast<std::int32_t>(std::floor(sx));
            const auto y0 = static_cast<std::int32_t>(std::floor(sy));
            if (x0 < 0 || y0 < 0 || x0 + 1 >= width || y0 + 1 >= height) {
                continue;  // 保留背景色
            }
            const double fx = sx - static_cast<double>(x0);
            const double fy = sy - static_cast<double>(y0);
            const std::uint8_t* p00 = base + stride * static_cast<std::size_t>(y0) + x0 * 4;
            const std::uint8_t* p10 = p00 + 4;
            const std::uint8_t* p01 = p00 + stride;
            const std::uint8_t* p11 = p01 + 4;
            for (int ch = 0; ch < 4; ++ch) {
                const double top = static_cast<double>(p00[ch]) * (1.0 - fx) +
                                   static_cast<double>(p10[ch]) * fx;
                const double bottom = static_cast<double>(p01[ch]) * (1.0 - fx) +
                                      static_cast<double>(p11[ch]) * fx;
                const double value = top * (1.0 - fy) + bottom * fy;
                dst[x * 4 + ch] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    return out;
}

PixelBuffer applyEnhancement(const PixelBuffer& source, const EnhanceSettings& settings) {
    if (source.isNull()) return PixelBuffer{};

    const std::array<std::uint8_t, 256> curve = domain::enhance::buildToneCurve(settings);
    const std::int32_t width = source.width();
    const std::int32_t height = source.height();
    PixelBuffer out(width, height);

    const bool needGray = settings.grayscale || settings.binarize != domain::enhance::BinarizeMode::None;

    for (std::int32_t y = 0; y < height; ++y) {
        const std::uint8_t* src = source.data() + source.stride() * static_cast<std::size_t>(y);
        std::uint8_t* dst = out.scanline(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const std::uint8_t b = curve[src[x * 4 + 0]];
            const std::uint8_t g = curve[src[x * 4 + 1]];
            const std::uint8_t r = curve[src[x * 4 + 2]];
            if (needGray) {
                const std::uint8_t value = luma(b, g, r);
                dst[x * 4 + 0] = value;
                dst[x * 4 + 1] = value;
                dst[x * 4 + 2] = value;
            } else {
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
            }
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }

    if (settings.binarize == domain::enhance::BinarizeMode::None) return out;

    std::uint8_t threshold = settings.fixedThreshold;
    if (settings.binarize == domain::enhance::BinarizeMode::Otsu) {
        // 門檻取自**已經套過色調曲線**的影像，而不是原圖：使用者調完對比之後
        // 看到的就是這張圖，門檻若算在原圖上，畫面與結果會對不起來。
        threshold = domain::enhance::otsuThreshold(grayHistogram(toGrayscale(out)));
    }

    for (std::int32_t y = 0; y < height; ++y) {
        std::uint8_t* dst = out.scanline(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const std::uint8_t value = dst[x * 4 + 0] < threshold ? 0 : 255;
            dst[x * 4 + 0] = value;
            dst[x * 4 + 1] = value;
            dst[x * 4 + 2] = value;
        }
    }
    return out;
}

}  // namespace alioth::engine::enhance
