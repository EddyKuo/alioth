#pragma once

// 基準影像比對（WBS 7.3、PRD §9「渲染回歸 — 基準影像比對（SSIM）」）。
//
// 為什麼是 SSIM 而不是逐像素相等：
//   同一台機器、同一版 PDFium 的輸出其實是逐位元組決定的，逐像素相等會通過。
//   但這個框架的目的是跨 PDFium 版本（每季升版）與跨平台（後續移植 macOS / Linux）
//   偵測渲染退化，而那兩者一定會在抗鋸齒邊緣產生 ±1~2 的差異。
//   逐像素相等在那個情境下會天天紅燈，紅燈天天被無視，等於沒有這道關卡。
//   PRD §9 指定 SSIM 正是為了這個理由，這裡照辦。
//
// 為什麼同時報告逐像素統計：
//   SSIM 對「整張圖平移一個像素」很敏感，對「一小塊區域整個不見了」反而不敏感
//   （8×8 視窗平均會稀釋掉）。因此門檻是 SSIM，但失敗訊息一定附上最大差值與
//   差異像素比例，讓人一眼看出是全域偏移還是局部缺塊。
//
// 門檻 0.995 的來源：實測同一機器同版 PDFium 的重複渲染 SSIM = 1.0，
// 抗鋸齒層級的差異約落在 0.998 以上，而少畫一條線這種真實退化會掉到 0.99 以下。
// 0.995 留了餘裕但仍能抓到真實退化。門檻若需要放寬，必須連同理由一起改。

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QString>

#include <algorithm>
#include <cmath>
#include <vector>

namespace alioth::test {

inline constexpr double kGoldenSsimThreshold = 0.995;

struct ImageComparison {
    bool sizeMatches{false};
    double ssim{0.0};
    int maxChannelDelta{0};
    double differingPixelRatio{0.0};

    [[nodiscard]] bool acceptable() const noexcept {
        return sizeMatches && ssim >= kGoldenSsimThreshold;
    }
};

namespace detail {

// Rec. 601 亮度。SSIM 在灰階上計算——彩色分別算三通道再平均，
// 對本案（工程圖幾乎全是灰階線條）只是三倍成本換不到資訊。
inline std::vector<double> toLuma(const QImage& image) {
    std::vector<double> luma;
    luma.resize(static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height()));
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) +
                 static_cast<std::size_t>(x)] = 0.299 * qRed(pixel) + 0.587 * qGreen(pixel) +
                                                0.114 * qBlue(pixel);
        }
    }
    return luma;
}

}  // namespace detail

// 全域 SSIM。8×8 視窗、步進 4（視窗重疊），常數用標準的 (0.01L)^2 與 (0.03L)^2。
inline double computeSsim(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.width() < 8 || a.height() < 8) return 0.0;

    const std::vector<double> lumaA = detail::toLuma(a);
    const std::vector<double> lumaB = detail::toLuma(b);
    const int width = a.width();
    const int height = a.height();

    constexpr double c1 = (0.01 * 255.0) * (0.01 * 255.0);
    constexpr double c2 = (0.03 * 255.0) * (0.03 * 255.0);
    constexpr int window = 8;
    constexpr int step = 4;

    double total = 0.0;
    int windows = 0;

    for (int y = 0; y + window <= height; y += step) {
        for (int x = 0; x + window <= width; x += step) {
            double sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
            for (int dy = 0; dy < window; ++dy) {
                const std::size_t row = static_cast<std::size_t>(y + dy) *
                                        static_cast<std::size_t>(width);
                for (int dx = 0; dx < window; ++dx) {
                    const double va = lumaA[row + static_cast<std::size_t>(x + dx)];
                    const double vb = lumaB[row + static_cast<std::size_t>(x + dx)];
                    sumA += va;
                    sumB += vb;
                    sumAA += va * va;
                    sumBB += vb * vb;
                    sumAB += va * vb;
                }
            }
            constexpr double n = window * window;
            const double meanA = sumA / n;
            const double meanB = sumB / n;
            const double varA = sumAA / n - meanA * meanA;
            const double varB = sumBB / n - meanB * meanB;
            const double covAB = sumAB / n - meanA * meanB;

            const double numerator = (2.0 * meanA * meanB + c1) * (2.0 * covAB + c2);
            const double denominator =
                (meanA * meanA + meanB * meanB + c1) * (varA + varB + c2);
            total += denominator > 0.0 ? numerator / denominator : 1.0;
            ++windows;
        }
    }

    return windows > 0 ? total / windows : 0.0;
}

inline ImageComparison compareImages(const QImage& actual, const QImage& golden) {
    ImageComparison comparison;
    comparison.sizeMatches = actual.size() == golden.size();
    if (!comparison.sizeMatches) return comparison;

    std::size_t differing = 0;
    const std::size_t total = static_cast<std::size_t>(actual.width()) *
                              static_cast<std::size_t>(actual.height());
    for (int y = 0; y < actual.height(); ++y) {
        const auto* lineA = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* lineB = reinterpret_cast<const QRgb*>(golden.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            const int delta = std::max({std::abs(qRed(lineA[x]) - qRed(lineB[x])),
                                        std::abs(qGreen(lineA[x]) - qGreen(lineB[x])),
                                        std::abs(qBlue(lineA[x]) - qBlue(lineB[x]))});
            if (delta > 0) ++differing;
            comparison.maxChannelDelta = std::max(comparison.maxChannelDelta, delta);
        }
    }
    comparison.differingPixelRatio =
        total > 0 ? static_cast<double>(differing) / static_cast<double>(total) : 0.0;
    comparison.ssim = computeSsim(actual, golden);
    return comparison;
}

// 差異圖：紅色標出不同的像素，底圖是淡化的實際輸出。
// 失敗時把它寫到 out/ 供人檢視——只給一個 SSIM 數字，沒人能判斷是什麼壞了。
inline QImage makeDiffImage(const QImage& actual, const QImage& golden) {
    if (actual.size() != golden.size()) return {};
    QImage diff(actual.size(), QImage::Format_ARGB32);
    for (int y = 0; y < actual.height(); ++y) {
        const auto* lineA = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* lineB = reinterpret_cast<const QRgb*>(golden.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(diff.scanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            const int delta = std::max({std::abs(qRed(lineA[x]) - qRed(lineB[x])),
                                        std::abs(qGreen(lineA[x]) - qGreen(lineB[x])),
                                        std::abs(qBlue(lineA[x]) - qBlue(lineB[x]))});
            if (delta > 0) {
                out[x] = qRgb(255, 0, 0);
            } else {
                const int grey = 200 + qGray(lineA[x]) / 5;
                out[x] = qRgb(grey, grey, grey);
            }
        }
    }
    return diff;
}

inline bool saveImage(const QString& path, const QImage& image) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return image.save(path, "PNG");
}

}  // namespace alioth::test
