#pragma once

// 量測領域模型（PRD-ANN-014 / 024 / 025 / 026、WBS 4.2 附掛量測）。
//
// 純 C++，不依賴 Qt 或 PDFium：比例換算與幾何計算是全案對「正確性」要求最嚴的
// 一段邏輯（工程審圖 Persona 的核心賣點），必須能在無 GUI、無引擎的環境下
// 密集測試邊界情況。
//
// 刻意不假設任何預設比例。PDF 的 /Measure 字典（ISO 32000-1 §12.5.6.11）若
// 不存在，代表這份文件從未被校正過；那種狀態下顯示「1:1」或任何看似合理的
// 數字都是比顯示「未校正」更糟的結果——使用者會把猜出來的假數字當真。
// 因此本檔的比例一律用「未校正」這個顯式狀態表達（unitsPerPoint <= 0），
// 呼叫端必須先檢查 isCalibrated() 才能取值。

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::domain {

// PDF 頁面座標的單位恆為點（1/72 吋）。量測結果要換算成的顯示單位。
enum class LengthUnit : std::uint8_t {
    Point,
    Millimetre,
    Centimetre,
    Metre,
    Inch,
    Foot,
};

[[nodiscard]] constexpr const char* unitLabel(LengthUnit unit) noexcept {
    switch (unit) {
        case LengthUnit::Point:       return "pt";
        case LengthUnit::Millimetre:  return "mm";
        case LengthUnit::Centimetre:  return "cm";
        case LengthUnit::Metre:       return "m";
        case LengthUnit::Inch:        return "in";
        case LengthUnit::Foot:        return "ft";
    }
    return "pt";
}

// 由 /Measure /X /U 這類字串鍵還原成 LengthUnit。認不得的標籤回傳
// std::nullopt，讓呼叫端把它當成「格式我們不支援」而不是硬猜一個單位。
[[nodiscard]] inline std::optional<LengthUnit> unitFromLabel(const std::string& label) noexcept {
    if (label == "pt") return LengthUnit::Point;
    if (label == "mm") return LengthUnit::Millimetre;
    if (label == "cm") return LengthUnit::Centimetre;
    if (label == "m") return LengthUnit::Metre;
    if (label == "in") return LengthUnit::Inch;
    if (label == "ft") return LengthUnit::Foot;
    return std::nullopt;
}

// 未經任何比例校正、單純的物理單位換算：一個 PDF 點列印出來實際佔多少該單位。
// 72 點 = 1 吋是 PDF 規格的定義值（§7.9.5），不是量測比例，因此獨立於
// MeasureInfo 之外——校正比例是「紙面距離 → 使用者定義的真實世界距離」，
// 物理換算是「紙面距離 → 紙面本身的實際尺寸」，兩者不是同一件事，
// calibrateUniform() 會把它們相乘。
[[nodiscard]] constexpr double physicalUnitsPerPoint(LengthUnit unit) noexcept {
    switch (unit) {
        case LengthUnit::Point:      return 1.0;
        case LengthUnit::Inch:       return 1.0 / 72.0;
        case LengthUnit::Millimetre: return 25.4 / 72.0;
        case LengthUnit::Centimetre: return 2.54 / 72.0;
        case LengthUnit::Metre:      return 0.0254 / 72.0;
        case LengthUnit::Foot:       return 1.0 / 72.0 / 12.0;
    }
    return 1.0;
}

// 一則量測註解的比例狀態，對應寫入 /Measure 字典所需的最小資訊。
//
// unitsPerPoint 是「頁面座標的一點，換算成 unitLabel 單位後的實際世界距離」，
// 也就是 /Measure /X 底下 NumberFormat 的 /C。<= 0 代表未校正。
struct MeasureInfo {
    std::string ratioLabel{};   // 人類可讀比例，如 "1:100"；對應 /Measure /R
    std::string unitLabel{};    // 顯示單位，如 "mm"；對應 /Measure /X /U
    double unitsPerPoint{0.0};  // <= 0 代表未校正

    [[nodiscard]] bool isCalibrated() const noexcept { return unitsPerPoint > 0.0 && std::isfinite(unitsPerPoint); }

    friend bool operator==(const MeasureInfo&, const MeasureInfo&) = default;
};

struct CalibrationResult {
    bool ok{false};
    std::string diagnostic{};   // ok 為 false 時說明原因
    MeasureInfo measure{};
};

// 格式化比例標籤。ratio 是「紙面實際尺寸 : 真實世界尺寸」，四捨五入到
// 三位有效數字——比例尺本來就是近似值，硬留浮點誤差只會讓標籤看起來很假
// （例如 "1:99.9997"）。
[[nodiscard]] inline std::string formatRatioLabel(double ratio) {
    if (!(ratio > 0.0) || !std::isfinite(ratio)) return "1:?";
    // 三位有效數字：先算量級，再四捨五入到對應小數位。
    const double magnitude = std::floor(std::log10(ratio));
    const double factor = std::pow(10.0, 2.0 - magnitude);
    double rounded = std::round(ratio * factor) / factor;

    // 去掉不必要的小數：量測比例尺幾乎都是整數（1:50、1:100、1:200）。
    if (std::abs(rounded - std::round(rounded)) < 1e-9) {
        return "1:" + std::to_string(static_cast<long long>(std::llround(rounded)));
    }
    std::string text = std::to_string(rounded);
    // std::to_string 固定六位小數，砍到三位有效數字對應的長度即可，
    // 尾端多餘的零與小數點一併清掉。
    const std::size_t dot = text.find('.');
    if (dot != std::string::npos) {
        std::size_t end = std::min(text.size(), dot + 4);
        text = text.substr(0, end);
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return "1:" + text;
}

// PRD-ANN-024：以「紙面上量到的兩點距離（點）」對應「使用者輸入的真實距離」
// 校正比例尺。這是使用者在圖面上點兩下、輸入實際長度（例如 1:100 圖上的
// 10 公尺）之後唯一需要的計算。
//
// 刻意拒絕非正值輸入而不是夾住：夾成一個「還能算」的數字，換算出來的一切
// 後續量測都會是看起來合理但錯誤的數字，比直接失敗更難查。
[[nodiscard]] inline CalibrationResult calibrateUniform(double pointDistance, double realDistance,
                                                         LengthUnit unit) {
    CalibrationResult result{};
    if (!(pointDistance > 0.0) || !std::isfinite(pointDistance)) {
        result.diagnostic = "校正基準的紙面距離必須大於零";
        return result;
    }
    if (!(realDistance > 0.0) || !std::isfinite(realDistance)) {
        result.diagnostic = "校正基準的實際距離必須大於零";
        return result;
    }

    const double paperPhysical = pointDistance * physicalUnitsPerPoint(unit);
    result.ok = true;
    result.measure.unitLabel = unitLabel(unit);
    result.measure.unitsPerPoint = realDistance / pointDistance;
    result.measure.ratioLabel = formatRatioLabel(realDistance / paperPhysical);
    return result;
}

// ---- 幾何計算：距離、周長、面積 ----

[[nodiscard]] inline double pointDistance(const PointF& a, const PointF& b) noexcept {
    return std::hypot(b.x - a.x, b.y - a.y);
}

// 折線總長：相鄰點距離逐段相加，不補回起點的閉合邊。
// PRD-ANN-026「折線→周長」沿用 Acrobat／PDF-XChange 的既有用語，實際計算的
// 是路徑總長，不是封閉圖形的周長——折線本身不保證閉合，替它加一條回到起點
// 的邊會量出使用者沒有畫的線段。
[[nodiscard]] inline double polylineLength(const std::vector<PointF>& points) noexcept {
    if (points.size() < 2) return 0.0;
    double total = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) total += pointDistance(points[i - 1], points[i]);
    return total;
}

// 多邊形周長：折線總長再補上回到起點的閉合邊。
[[nodiscard]] inline double polygonPerimeter(const std::vector<PointF>& points) noexcept {
    if (points.size() < 2) return 0.0;
    return polylineLength(points) + pointDistance(points.back(), points.front());
}

// Shoelace 公式的帶號面積（未取絕對值）。正負號代表繞行方向，量測不關心方向，
// 因此 polygonArea() 一律回傳絕對值。
[[nodiscard]] inline double signedShoelaceArea(const std::vector<PointF>& points) noexcept {
    if (points.size() < 3) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const PointF& a = points[i];
        const PointF& b = points[(i + 1) % points.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum * 0.5;
}

namespace detail {

// 判斷兩條線段 (a1,a2) 與 (b1,b2) 是否「真交叉」（不含共用端點或共線重疊的
// 退化情況——多邊形相鄰邊本來就共用一個端點，那不算自相交）。
// 標準的方向測試（叉積正負號），O(1)。
[[nodiscard]] inline double cross(const PointF& o, const PointF& a, const PointF& b) noexcept {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

[[nodiscard]] inline bool segmentsProperlyIntersect(const PointF& a1, const PointF& a2,
                                                     const PointF& b1, const PointF& b2) noexcept {
    const double d1 = cross(b1, b2, a1);
    const double d2 = cross(b1, b2, a2);
    const double d3 = cross(a1, a2, b1);
    const double d4 = cross(a1, a2, b2);
    constexpr double kEps = 1e-9;
    const bool straddle1 = (d1 > kEps && d2 < -kEps) || (d1 < -kEps && d2 > kEps);
    const bool straddle2 = (d3 > kEps && d4 < -kEps) || (d3 < -kEps && d4 > kEps);
    return straddle1 && straddle2;
}

}  // namespace detail

// 偵測多邊形（依序連接、最後回到起點）是否自相交。
//
// O(n^2)：量測用的多邊形是使用者手動點出來的頂點，規模是十位數，
// 不需要掃描線演算法的複雜度。只檢查「不相鄰」的邊——相鄰邊共用端點，
// 那是多邊形的正常構造，不是自相交。
[[nodiscard]] inline bool isPolygonSelfIntersecting(const std::vector<PointF>& points) noexcept {
    const std::size_t n = points.size();
    if (n < 4) return false;  // 三角形不可能自相交
    for (std::size_t i = 0; i < n; ++i) {
        const PointF& a1 = points[i];
        const PointF& a2 = points[(i + 1) % n];
        for (std::size_t j = i + 1; j < n; ++j) {
            // 相鄰邊（含首尾相接那一對）共用端點，跳過。
            if (j == i || j == (i + 1) % n || (j + 1) % n == i) continue;
            const PointF& b1 = points[j];
            const PointF& b2 = points[(j + 1) % n];
            if (detail::segmentsProperlyIntersect(a1, a2, b1, b2)) return true;
        }
    }
    return false;
}

struct AreaResult {
    double area{0.0};           // 恆為非負值
    bool selfIntersecting{false};
};

// 多邊形面積。
//
// 自相交多邊形的行為明確定義為：仍然回傳 shoelace 公式算出的帶號面積之絕對值，
// 但 selfIntersecting 設為 true。這個數字**不是**多邊形視覺上圈起來的面積——
// 自相交會讓某些區域被算兩次、某些區域正負抵消——但這是唯一不需要多邊形
// 布林運算就能定義的行為，而且是確定性、可測試的。呼叫端（app 層／UI）
// 必須檢查 selfIntersecting 並提示使用者，不能把這個數字當成可信賴的面積
// 靜默顯示出來（CLAUDE.md IL-4：降級必須讓使用者看得見）。
[[nodiscard]] inline AreaResult computePolygonArea(const std::vector<PointF>& points) {
    AreaResult result{};
    if (points.size() < 3) return result;
    result.area = std::abs(signedShoelaceArea(points));
    result.selfIntersecting = isPolygonSelfIntersecting(points);
    return result;
}

// ---- 套用比例後的量測結果 ----

struct MeasurementResult {
    bool ok{false};
    std::string diagnostic{};   // 未校正或輸入不足時的原因
    double value{0.0};
    std::string unitLabel{};    // 面積會附加上標平方，例如 "mm2"（純 ASCII，避免上標字元的編碼問題）
    bool selfIntersecting{false};  // 僅面積量測有意義
};

[[nodiscard]] inline MeasurementResult measureDistance(const PointF& a, const PointF& b,
                                                        const MeasureInfo& scale) {
    MeasurementResult result{};
    if (!scale.isCalibrated()) {
        result.diagnostic = "此頁尚未校正比例（Calibrate），無法換算實際距離";
        return result;
    }
    result.ok = true;
    result.value = pointDistance(a, b) * scale.unitsPerPoint;
    result.unitLabel = scale.unitLabel;
    return result;
}

[[nodiscard]] inline MeasurementResult measurePerimeter(const std::vector<PointF>& points,
                                                         const MeasureInfo& scale, bool closed) {
    MeasurementResult result{};
    if (!scale.isCalibrated()) {
        result.diagnostic = "此頁尚未校正比例（Calibrate），無法換算實際長度";
        return result;
    }
    result.ok = true;
    result.value = (closed ? polygonPerimeter(points) : polylineLength(points)) * scale.unitsPerPoint;
    result.unitLabel = scale.unitLabel;
    return result;
}

[[nodiscard]] inline MeasurementResult measureArea(const std::vector<PointF>& points,
                                                    const MeasureInfo& scale) {
    MeasurementResult result{};
    if (!scale.isCalibrated()) {
        result.diagnostic = "此頁尚未校正比例（Calibrate），無法換算實際面積";
        return result;
    }
    const AreaResult area = computePolygonArea(points);
    result.ok = true;
    result.value = area.area * scale.unitsPerPoint * scale.unitsPerPoint;
    result.unitLabel = scale.unitLabel + "2";
    result.selfIntersecting = area.selfIntersecting;
    return result;
}

// 非等向縮放偵測：/Measure 可以分別給 X 軸與 Y 軸不同的 /C（例如掃描時
// 長寬被不等比縮放的圖面）。這種文件的距離／面積換算沒有單一正確係數，
// 若簡單取其中一軸硬算，會產生看起來合理、實際上系統性錯誤的數字。
// 因此一律拒絕，而不是挑一個軸將就。
[[nodiscard]] inline std::optional<double> resolveUniformScale(double unitsPerPointX,
                                                                double unitsPerPointY,
                                                                double relativeEpsilon = 1e-6) noexcept {
    if (!(unitsPerPointX > 0.0) || !std::isfinite(unitsPerPointX)) return std::nullopt;
    if (!(unitsPerPointY > 0.0) || !std::isfinite(unitsPerPointY)) return unitsPerPointX;
    const double diff = std::abs(unitsPerPointX - unitsPerPointY);
    const double scale = std::max(std::abs(unitsPerPointX), std::abs(unitsPerPointY));
    if (scale > 0.0 && diff / scale > relativeEpsilon) return std::nullopt;
    return unitsPerPointX;
}

}  // namespace alioth::domain
