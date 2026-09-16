#pragma once

// 掃描增強的領域模型（WBS 14，PRD-ENH-001 ~ 004）。
//
// 這裡只放「描述要做什麼」的資料與純數學，不放任何像素迴圈，也不碰 PDF。
// 分開的理由是這四項功能的錯誤幾乎都出在參數語意上——對比要不要先減中點、
// 角度是順時針還是逆時針、重壓縮到底該不該換掉原圖——而那些判斷用一組
// 純量就能驗完，不需要生一張圖或一份 PDF。像素緩衝進出的演算法在
// engine/enhance/image_ops.h，PDF 讀寫在同目錄的其他檔案。
//
// 領域層規則：純 C++，不得依賴 Qt 或 PDFium（SDD §2.1）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::domain::enhance {

// ---------------------------------------------------------------------------
// 去斜（PRD-ENH-002 的偵測側）
// ---------------------------------------------------------------------------

// 角度一律以「度」表示，正值代表影像內容相對水平線逆時針傾斜；
// 校正時要施加的旋轉是它的相反數。用度而不是弧度是因為所有的門檻值
// （0.1 度視為水平、最大 15 度）在使用者介面上都以度呈現，
// 在領域層就換算成弧度只會讓兩邊的常數對不起來。
struct DeskewSettings {
    // 掃描件的傾斜幾乎都來自送紙偏移，實測範圍在 ±5 度內；給到 15 度已經很寬。
    // 放大搜尋範圍的代價不只是時間：角度越大，投影剖面的次高峰越容易勝出，
    // 誤判率跟著上升。
    double maxAngleDeg{15.0};

    // 兩階段搜尋。粗掃定位大致方向，細掃在粗掃結果附近以更小步長收斂。
    // 單階段用 0.05 度掃 ±15 度要算 600 次投影，對 A4 300dpi 的圖是秒級成本。
    double coarseStepDeg{1.0};
    double fineStepDeg{0.05};

    // 小於這個角度視為本來就是正的。這條門檻的存在理由是「不要把好的弄壞」：
    // 旋轉一定會重新取樣，文字邊緣必然變糊；為了 0.05 度的傾斜付出那個代價
    // 是淨損失。誤判成傾斜比漏判嚴重得多，因此門檻寧可偏高。
    double minApplyAngleDeg{0.15};

    // 分數提升門檻：最佳角度的分數必須比零度高出這個比例才算「真的有傾斜」。
    // 沒有這一條的話，純圖片頁或空白頁的分數曲線幾乎是平的，
    // 最佳角度會由雜訊決定，結果是把正常掃描件轉歪。
    double minScoreGain{0.02};

    // 分析前把影像縮到這個最長邊。投影剖面法只需要行結構，不需要字形細節；
    // 縮小同時抑制了高頻雜訊，準確度反而更好。
    std::int32_t analysisMaxEdge{1000};
};

struct DeskewResult {
    bool detected{false};      // 是否判定為傾斜（已套用 minApplyAngleDeg 與門檻）
    double angleDeg{0.0};      // 偵測到的傾斜角，正值為逆時針
    double scoreGain{0.0};     // 最佳分數相對零度的提升比例
    std::string note{};        // 未判定為傾斜時的原因，供 UI 顯示而非靜默忽略
};

// 校正需要施加的旋轉角。內容逆時針歪了就要順時針轉回去。
[[nodiscard]] inline double correctionAngleDeg(const DeskewResult& result) noexcept {
    return result.detected ? -result.angleDeg : 0.0;
}

[[nodiscard]] inline bool settingsValid(const DeskewSettings& s) noexcept {
    return s.maxAngleDeg > 0.0 && s.maxAngleDeg <= 45.0 && s.coarseStepDeg > 0.0 &&
           s.fineStepDeg > 0.0 && s.fineStepDeg <= s.coarseStepDeg && s.minApplyAngleDeg >= 0.0 &&
           s.analysisMaxEdge >= 64;
}

// ---------------------------------------------------------------------------
// 增強（PRD-ENH-002 的影像側）
// ---------------------------------------------------------------------------

enum class BinarizeMode : std::uint8_t {
    None,
    Fixed,   // 用 fixedThreshold
    Otsu,    // 由直方圖自動決定
};

struct EnhanceSettings {
    // 亮度以 8 位元的絕對量表示（-255 ~ +255），對使用者而言比乘法直觀：
    // 「整頁偏暗，加 20」是可預期的操作，而乘 1.1 在暗部幾乎沒有效果。
    double brightness{0.0};

    // 對比為 -100 ~ +100 的百分比。轉成乘數時繞著 128 這個中點，
    // 否則提高對比會同時整體變亮，使用者會以為亮度也被動過了。
    double contrast{0.0};

    bool grayscale{false};
    BinarizeMode binarize{BinarizeMode::None};
    std::uint8_t fixedThreshold{128};
};

// 對比百分比 → 乘數。採 (259 * (c + 255)) / (255 * (259 - c)) 這條在影像處理裡
// 通用的曲線：它在 c = 0 時剛好等於 1，在 ±100 時給出合理的極值，
// 而單純用 1 + c/100 在負值端會直接壓成全灰。
[[nodiscard]] inline double contrastFactor(double contrast) noexcept {
    const double c = std::clamp(contrast, -100.0, 100.0);
    return (259.0 * (c + 255.0)) / (255.0 * (259.0 - c));
}

// 單一通道的色調映射。
//
// 這個函式是本模組最容易寫錯的一行：中間值必須以浮點或寬整數計算後再夾回
// 0–255。用 std::uint8_t 累加的話 255 + 10 會回繞成 9，症狀是亮部出現黑點，
// 看起來像雜訊而不像溢位，極難從畫面反推原因。
[[nodiscard]] inline std::uint8_t applyTone(std::uint8_t value,
                                            const EnhanceSettings& settings) noexcept {
    const double factor = contrastFactor(settings.contrast);
    const double centred = factor * (static_cast<double>(value) - 128.0) + 128.0;
    const double shifted = centred + settings.brightness;
    const double rounded = std::round(shifted);
    if (rounded <= 0.0) return 0;
    if (rounded >= 255.0) return 255;
    return static_cast<std::uint8_t>(rounded);
}

// 256 項查表。每個像素重算一次 contrastFactor 是純粹的浪費，
// 而查表也保證了「同樣的輸入值必得同樣的輸出」，不會因為編譯器的浮點重排
// 在不同最佳化等級下產生差一階的結果。
[[nodiscard]] inline std::array<std::uint8_t, 256> buildToneCurve(
    const EnhanceSettings& settings) noexcept {
    std::array<std::uint8_t, 256> curve{};
    for (std::size_t i = 0; i < curve.size(); ++i) {
        curve[i] = applyTone(static_cast<std::uint8_t>(i), settings);
    }
    return curve;
}

// 由 256 格直方圖求 Otsu 門檻（類間變異數最大化）。
//
// 放在領域層是因為它只是直方圖的統計，與像素怎麼排列無關；
// 這讓它可以用手寫的直方圖驗到邊界（全黑、全白、雙峰）而不必造圖。
// 單峰輸入（全黑或全白）沒有真正的門檻可言，此時回傳 128 並不會造成問題：
// 呼叫端拿它去二值化，全黑仍是全黑、全白仍是全白。
//
// 回傳值的語意是「小於它的算暗部」，因此在最佳分割點上加一。差這個一
// 的後果不是畫面差一階灰，而是雙峰直方圖中較暗的那一峰整個被歸到亮部——
// 淡墨掃描件會整頁變白。
[[nodiscard]] inline std::uint8_t otsuThreshold(const std::array<std::uint64_t, 256>& histogram) noexcept {
    std::uint64_t total = 0;
    double sum = 0.0;
    for (std::size_t i = 0; i < histogram.size(); ++i) {
        total += histogram[i];
        sum += static_cast<double>(i) * static_cast<double>(histogram[i]);
    }
    if (total == 0) return 128;

    std::uint64_t weightBackground = 0;
    double sumBackground = 0.0;
    double bestVariance = -1.0;
    int bestThreshold = 128;
    for (int t = 0; t < 256; ++t) {
        weightBackground += histogram[static_cast<std::size_t>(t)];
        if (weightBackground == 0) continue;
        const std::uint64_t weightForeground = total - weightBackground;
        if (weightForeground == 0) break;

        sumBackground += static_cast<double>(t) * static_cast<double>(histogram[static_cast<std::size_t>(t)]);
        const double meanBackground = sumBackground / static_cast<double>(weightBackground);
        const double meanForeground =
            (sum - sumBackground) / static_cast<double>(weightForeground);
        const double delta = meanBackground - meanForeground;
        const double variance =
            static_cast<double>(weightBackground) * static_cast<double>(weightForeground) * delta * delta;
        if (variance > bestVariance) {
            bestVariance = variance;
            bestThreshold = t;
        }
    }
    if (bestVariance < 0.0) return 128;  // 單峰：沒有可分的兩群
    return static_cast<std::uint8_t>(std::clamp(bestThreshold + 1, 0, 255));
}

// ---------------------------------------------------------------------------
// 影像編碼（點陣化與重壓縮共用）
// ---------------------------------------------------------------------------

enum class ImageCodec : std::uint8_t {
    Jpeg,   // /DCTDecode，掃描頁的預設
    Flate,  // /FlateDecode，線稿與已二值化的內容明顯更小也不失真
    Auto,   // 兩種都編，取小的
};

struct CompressionSettings {
    ImageCodec codec{ImageCodec::Auto};

    // JPEG 品質 1–100。掃描文件在 75 附近是體積與可讀性的轉折點，
    // 再往下文字邊緣的振鈴會開始吃掉筆畫。
    std::int32_t jpegQuality{75};

    // 影像重取樣的目標 dpi。0 代表不重取樣。
    // 重取樣通常比調品質更有效：600 dpi 的掃描件降到 200 dpi 就少掉九成像素。
    double targetDpi{0.0};

    [[nodiscard]] bool valid() const noexcept {
        return jpegQuality >= 1 && jpegQuality <= 100 && targetDpi >= 0.0;
    }
};

// ---------------------------------------------------------------------------
// 影像重壓縮（PRD-ENH-004）
// ---------------------------------------------------------------------------

// 為什麼需要「不換」這個結果：重壓縮之後變大是常見而非例外。
// 原圖若已經是高品質 JPEG，再解一次、編一次幾乎必然變大（世代損失還會累積）；
// 已經是 CCITT 或 JBIG2 的黑白掃描件，轉成 JPEG 更是好幾倍。
// 因此「變小才替換」不是最佳化，是正確性要求——使用者按下重壓縮之後
// 檔案變大，那是產品缺陷。
enum class RecompressDecision : std::uint8_t {
    Replaced,          // 新資料較小，已替換
    KeptLarger,        // 重壓後較大，保留原圖
    KeptBelowSaving,   // 有變小但省得不夠多，不值得重新編碼帶來的畫質損失
    SkippedUnsupported,// 濾鏡或色彩空間不支援，明確跳過而不是靜默略過
    SkippedTooSmall,   // 小圖（圖示、線條）重壓的收益低於風險
    Failed,            // 解碼或編碼失敗
};

[[nodiscard]] inline const char* describe(RecompressDecision decision) noexcept {
    switch (decision) {
        case RecompressDecision::Replaced:           return "已替換";
        case RecompressDecision::KeptLarger:         return "重壓後變大，保留原圖";
        case RecompressDecision::KeptBelowSaving:    return "節省幅度未達門檻，保留原圖";
        case RecompressDecision::SkippedUnsupported: return "不支援的濾鏡或色彩空間";
        case RecompressDecision::SkippedTooSmall:    return "影像過小，略過";
        case RecompressDecision::Failed:             return "解碼或編碼失敗";
    }
    return "未知";
}

struct RecompressSettings {
    CompressionSettings compression{};

    // 至少要省下這個比例才值得替換。門檻不是 0：重新編碼是有損的，
    // 為了 1% 的體積犧牲一代畫質是壞交易。
    double minSavingRatio{0.05};

    // 像素數低於此值的影像不動。小圖多半是圖示或標誌，
    // JPEG 在小尺寸上的區塊效應相對明顯，而省下的位元組微不足道。
    std::int64_t minPixelCount{64 * 64};

    // 這裡曾有一個 includeMasks 開關。移除的理由是重壓縮不改尺寸，因此
    // /SMask 與間接 /Mask 一律原樣保留——兩種設定行為完全相同的旗標，
    // 對呼叫端而言比沒有這個旗標更糟：它看起來可以關掉遮罩處理，實際上
    // 什麼都不會發生。重取樣路徑真的需要時再加，屆時它會有真正的分支。

    // 遮罩本身一律無損。把 alpha 通道編成 JPEG 會在邊緣產生半透明的振鈴，
    // 表現為物件周圍的一圈灰邊。
    bool masksLosslessOnly{true};
};

struct ImageRecompressReport {
    std::string resourceName{};   // /XObject 底下的名稱，例如 Im0
    int objectNumber{0};
    std::int32_t width{0};
    std::int32_t height{0};
    std::int64_t originalBytes{0};
    std::int64_t newBytes{0};     // 未替換時仍記錄，讓使用者看得到差多少
    bool isMask{false};
    RecompressDecision decision{RecompressDecision::Failed};

    [[nodiscard]] std::int64_t savedBytes() const noexcept {
        return decision == RecompressDecision::Replaced ? originalBytes - newBytes : 0;
    }
};

// 替換與否的判斷。抽成純函數是因為這是本功能唯一的商業規則，
// 而它的邊界（剛好相等、剛好差一個位元組）用數字驗比用真圖驗可靠得多。
[[nodiscard]] inline RecompressDecision decideReplacement(std::int64_t originalBytes,
                                                          std::int64_t newBytes,
                                                          const RecompressSettings& settings) noexcept {
    if (originalBytes <= 0 || newBytes <= 0) return RecompressDecision::Failed;
    if (newBytes >= originalBytes) return RecompressDecision::KeptLarger;
    const double saving =
        static_cast<double>(originalBytes - newBytes) / static_cast<double>(originalBytes);
    if (saving < settings.minSavingRatio) return RecompressDecision::KeptBelowSaving;
    return RecompressDecision::Replaced;
}

// ---------------------------------------------------------------------------
// 頁面點陣化（PRD-ENH-003）
// ---------------------------------------------------------------------------

struct RasterizeSettings {
    // 150 dpi 是螢幕閱讀的下限，300 dpi 才印得出來。預設取 150：
    // 點陣化的主要用途是「凍結版面、避免對方重排」，不是製版。
    double dpi{150.0};

    CompressionSettings compression{};

    // 點陣化的是**內容**不是**標記**。註解是獨立的物件、不在內容串流裡，
    // 因此保留它們不需要額外工作——但把這件事寫成明確的旗標，
    // 是為了讓「點陣化之後便利貼不見了」這種錯誤在讀程式碼時就被看見。
    bool keepAnnotations{true};

    [[nodiscard]] bool valid() const noexcept {
        return dpi >= 36.0 && dpi <= 1200.0 && compression.valid();
    }
};

// 頁面尺寸（點）換算成指定 dpi 下的像素數。
// 1 點 = 1/72 吋，所以像素 = 點 / 72 * dpi。
[[nodiscard]] inline std::int32_t pixelsForPoints(double points, double dpi) noexcept {
    if (points <= 0.0 || dpi <= 0.0) return 0;
    const double pixels = std::floor(points / 72.0 * dpi + 0.5);
    if (pixels < 1.0) return 1;
    // 上限對應 A0 在 1200 dpi 下的邊長；再大的分配會直接把記憶體吃光，
    // 而使用者要的是一張圖不是一次當機。
    constexpr double kMaxEdge = 60000.0;
    return static_cast<std::int32_t>(std::min(pixels, kMaxEdge));
}

// ---------------------------------------------------------------------------
// 色彩轉換與 Recolor（PRD-ENH-006）
// ---------------------------------------------------------------------------
//
// **覆蓋範圍必須明講，這是本功能唯一容易被誤解為「全部變灰但其實漏了一半」
// 的地方**：PDF 的顏色來源分散在至少四個地方——內容串流的裝置色彩運算子
// （g/G/rg/RG/k/K/sc/scn 等）、影像 XObject 的樣本值、以 /ColorSpace 參照的
// Separation/DeviceN/ICCBased/Indexed 色彩空間、以及 /ExtGState 裡的透明度群組。
// 本功能覆蓋前兩者；後兩者一律略過並在報告中列出「未覆蓋」筆數，理由如下：
//
//   - Separation/DeviceN 的顏色由「著色劑名稱 + tint transform 函數」決定，
//     要正確轉換必須先執行那個函數（PostScript 計算函數或取樣函數），
//     這是一個小型直譯器，複雜度與本工作包其餘五項需求相當
//   - ICCBased 只給了 profile 位元組，沒有色彩管理引擎就無法正確轉成
//     螢幕可比較的 RGB／灰階值；瞎猜會產生「看起來對但色偏」的結果，
//     比明確不處理更容易誤導使用者
//   - Indexed 色彩空間的替換需要先展開調色盤再重建索引，屬於影像重編碼的
//     範疇而非單純的數值運算
//   - /ExtGState 的混合模式與透明度群組會讓最終畫面顏色不等於內容串流裡
//     寫的那個數值（例如 Multiply 混合），本功能只改「設定的顏色」，
//     不模擬混合結果
//
// 對 sc/scn（可變運算元數的通用設色運算子）採啟發式：只在運算元恰好是
// 1、3 或 4 個純數字（未帶色彩空間名稱、未帶 Pattern 名稱）時，分別視為
// DeviceGray／DeviceRGB／DeviceCMYK 處理。這不追蹤實際生效的 /ColorSpace
// （由先前的 cs/CS 運算子決定），因此如果文件真的用 4 個數字的 Indexed
// 或 ICCBased 色彩空間（極罕見但合法），會被誤判為 DeviceCMYK。
// 這個假設在程式與報告裡都要看得見，不能只藏在程式碼註解裡。
enum class ColorTransformMode : std::uint8_t {
    Grayscale,    // 全部轉為灰階（BT.601 亮度）
    Desaturate,   // 依 amount 在原色與灰階之間內插，0 = 不變、1 = 等同 Grayscale
    ReplaceColor, // 把落在容許誤差內的顏色替換成指定顏色
};

struct ColorTransformSettings {
    ColorTransformMode mode{ColorTransformMode::Grayscale};

    // Desaturate 專用：0–1，1 等同全灰階。
    double desaturateAmount{1.0};

    // ReplaceColor 專用。顏色以 0–1 的 RGB 表示，與 domain::ColorRgb 一致。
    ColorRgb replaceFrom{0.0, 0.0, 0.0};
    ColorRgb replaceTo{0.0, 0.0, 0.0};
    // 容許誤差：RGB 三分量各自的最大絕對差，而非歐氏距離——三個獨立門檻
    // 比距離公式更容易讓使用者理解「多接近算接近」，也不必為了距離門檻
    // 另外決定要不要開根號。
    double replaceTolerance{0.08};

    bool transformImages{true};
    bool transformVectorGraphics{true};

    // 空代表全部頁面。
    std::vector<std::int32_t> pages{};

    [[nodiscard]] bool valid() const noexcept {
        if (mode == ColorTransformMode::Desaturate &&
            (desaturateAmount < 0.0 || desaturateAmount > 1.0)) {
            return false;
        }
        if (mode == ColorTransformMode::ReplaceColor &&
            (replaceTolerance < 0.0 || replaceTolerance > 1.0)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool appliesToPage(std::int32_t index) const {
        if (pages.empty()) return true;
        return std::find(pages.begin(), pages.end(), index) != pages.end();
    }
};

// BT.601 亮度，浮點版本（0–1 輸入輸出）。與 engine/enhance/image_ops.cpp 的
// 定點版本使用同一組權重，確保向量顏色與點陣影像轉換後的灰階視覺一致。
[[nodiscard]] inline double luminance601(const ColorRgb& c) noexcept {
    return c.r * 0.299 + c.g * 0.587 + c.b * 0.114;
}

[[nodiscard]] inline ColorRgb clampColor(ColorRgb c) noexcept {
    c.r = std::clamp(c.r, 0.0, 1.0);
    c.g = std::clamp(c.g, 0.0, 1.0);
    c.b = std::clamp(c.b, 0.0, 1.0);
    return c;
}

[[nodiscard]] inline bool colorWithinTolerance(const ColorRgb& a, const ColorRgb& b,
                                               double tolerance) noexcept {
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance &&
           std::abs(a.b - b.b) <= tolerance;
}

// 套用色彩轉換到單一顏色。這是本功能唯一的商業邏輯，抽成純函數是因為
// 「灰階、去飽和、換色」三種模式的邊界（容許誤差剛好壓線、amount 為 0 或 1）
// 用數字驗證比每次都做一張圖或一份 PDF 可靠得多。
[[nodiscard]] inline ColorRgb applyColorTransform(const ColorRgb& input,
                                                  const ColorTransformSettings& settings) noexcept {
    switch (settings.mode) {
        case ColorTransformMode::Grayscale: {
            const double gray = luminance601(input);
            return ColorRgb{gray, gray, gray};
        }
        case ColorTransformMode::Desaturate: {
            const double gray = luminance601(input);
            const double amount = std::clamp(settings.desaturateAmount, 0.0, 1.0);
            return clampColor(ColorRgb{input.r + (gray - input.r) * amount,
                                      input.g + (gray - input.g) * amount,
                                      input.b + (gray - input.b) * amount});
        }
        case ColorTransformMode::ReplaceColor: {
            if (colorWithinTolerance(input, settings.replaceFrom, settings.replaceTolerance)) {
                return settings.replaceTo;
            }
            return input;
        }
    }
    return input;
}

// ---------------------------------------------------------------------------
// 新增背景（PRD-ENH-001）
// ---------------------------------------------------------------------------

enum class BackgroundSource : std::uint8_t {
    SolidColor,
    Image,
};

// 影像相對頁面的擺放方式。
enum class BackgroundFit : std::uint8_t {
    Stretch,  // 填滿，不保持比例
    Fit,      // 保持比例，完整放進頁面（可能留白）
    Fill,     // 保持比例，覆蓋整頁（可能裁切）
    Actual,   // 依 scale 以原始像素尺寸換算
};

// 水平與垂直的對齊。Fit / Actual 會留白，留白往哪邊靠必須說清楚。
enum class BackgroundAnchor : std::uint8_t {
    TopLeft, TopCenter, TopRight,
    MiddleLeft, Center, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
};

struct BackgroundSpec {
    BackgroundSource source{BackgroundSource::SolidColor};

    ColorRgb color{1.0, 1.0, 1.0};

    // 影像來源的原始位元組（PNG / JPEG 等由編碼層辨識）。純色時為空。
    std::vector<std::uint8_t> imageBytes{};

    BackgroundFit fit{BackgroundFit::Fit};
    BackgroundAnchor anchor{BackgroundAnchor::Center};

    // 0–1。背景通常要壓到 0.2 以下才不會妨礙閱讀，但預設仍給 1.0：
    // 純色背景（例如整頁淡黃）不該被偷偷變淡。
    double opacity{1.0};

    double rotationDeg{0.0};
    double scale{1.0};  // 僅 Actual 有意義

    // 四邊留白（點）。背景通常要避開裝訂邊。
    double marginLeft{0.0};
    double marginBottom{0.0};
    double marginRight{0.0};
    double marginTop{0.0};

    // 空代表全部頁面。頁碼為 0 起算，與引擎層一致。
    std::vector<std::int32_t> pages{};

    [[nodiscard]] bool valid() const noexcept {
        if (opacity < 0.0 || opacity > 1.0) return false;
        if (scale <= 0.0) return false;
        if (source == BackgroundSource::Image && imageBytes.empty()) return false;
        return true;
    }

    [[nodiscard]] bool appliesToPage(std::int32_t index) const {
        if (pages.empty()) return true;
        return std::find(pages.begin(), pages.end(), index) != pages.end();
    }
};

// 背景在頁面上的落點。
//
// 背景與浮水印的差別只有一個字：背景在內容**之下**（先畫），浮水印在**之上**
// （後畫）。幾何完全共用，因此這段計算刻意做成純函數並單獨測——
// 它算錯的症狀是圖偏一點點，在小樣張上看不出來，換成大圖才發現。
[[nodiscard]] inline RectF backgroundPlacement(const SizeF& pageSize, const SizeF& imageSize,
                                               const BackgroundSpec& spec) {
    const double left = spec.marginLeft;
    const double bottom = spec.marginBottom;
    const double right = pageSize.width - spec.marginRight;
    const double top = pageSize.height - spec.marginTop;
    const double boxWidth = right - left;
    const double boxHeight = top - bottom;
    if (boxWidth <= 0.0 || boxHeight <= 0.0) return RectF{};

    double width = boxWidth;
    double height = boxHeight;
    if (spec.source == BackgroundSource::Image && !imageSize.isEmpty()) {
        const double ratio = imageSize.width / imageSize.height;
        switch (spec.fit) {
            case BackgroundFit::Stretch:
                break;
            case BackgroundFit::Fit: {
                const double scale = std::min(boxWidth / imageSize.width, boxHeight / imageSize.height);
                width = imageSize.width * scale;
                height = imageSize.height * scale;
                break;
            }
            case BackgroundFit::Fill: {
                const double scale = std::max(boxWidth / imageSize.width, boxHeight / imageSize.height);
                width = imageSize.width * scale;
                height = imageSize.height * scale;
                break;
            }
            case BackgroundFit::Actual:
                // 影像像素以 72 dpi 視為點，再乘上使用者給的倍率。
                width = imageSize.width * spec.scale;
                height = width / ratio;
                break;
        }
    }

    double x = left;
    double y = bottom;
    switch (spec.anchor) {
        case BackgroundAnchor::TopLeft:
        case BackgroundAnchor::MiddleLeft:
        case BackgroundAnchor::BottomLeft:
            x = left;
            break;
        case BackgroundAnchor::TopCenter:
        case BackgroundAnchor::Center:
        case BackgroundAnchor::BottomCenter:
            x = left + (boxWidth - width) / 2.0;
            break;
        case BackgroundAnchor::TopRight:
        case BackgroundAnchor::MiddleRight:
        case BackgroundAnchor::BottomRight:
            x = right - width;
            break;
    }
    switch (spec.anchor) {
        case BackgroundAnchor::BottomLeft:
        case BackgroundAnchor::BottomCenter:
        case BackgroundAnchor::BottomRight:
            y = bottom;
            break;
        case BackgroundAnchor::MiddleLeft:
        case BackgroundAnchor::Center:
        case BackgroundAnchor::MiddleRight:
            y = bottom + (boxHeight - height) / 2.0;
            break;
        case BackgroundAnchor::TopLeft:
        case BackgroundAnchor::TopCenter:
        case BackgroundAnchor::TopRight:
            y = top - height;
            break;
    }

    return RectF{x, y, x + width, y + height};
}

}  // namespace alioth::domain::enhance
