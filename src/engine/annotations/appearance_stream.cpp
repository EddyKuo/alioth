#include "engine/annotations/appearance_stream.h"

#include <cstdio>

#include "engine/fonts/cjk_font_library.h"
#include "engine/fonts/text_runs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <variant>

#include "engine/annotations/text_layout.h"

namespace alioth::engine::annotations {

using domain::Annotation;
using domain::AnnotationType;
using domain::ColorRgb;
using domain::PointF;
using domain::QuadPoint;
using domain::RectF;

namespace {

// 文字標記的比例常數，全部以該行文字高度為基準。
//
// 用比例而非固定點數，是因為同一份文件裡標題與註腳的字高可以差三倍，
// 固定線寬會讓小字的底線粗到蓋住字。數值取自對 Acrobat 產出的目視比對。
constexpr double kMarkupThicknessRatio = 1.0 / 16.0;
constexpr double kMinMarkupThickness = 0.5;
constexpr double kUnderlineOffsetRatio = 1.0 / 16.0;
constexpr double kStrikeOutOffsetRatio = 0.5;
constexpr double kSquigglyAmplitudeRatio = 0.08;
constexpr double kSquigglyPeriodRatio = 0.25;

// 以四段三次貝茲逼近橢圓的控制點係數。誤差約萬分之二，遠低於 PDF 的顯示精度。
constexpr double kEllipseKappa = 0.5522847498307936;

constexpr double kDefaultNoteSize = 20.0;
constexpr double kMinArrowLength = 4.0;
constexpr double kArrowLengthRatio = 3.0;
constexpr double kArrowHalfWidthRatio = 0.4;

[[nodiscard]] double clampUnit(double v) noexcept {
    if (!(v > 0.0)) return 0.0;  // NaN 一併落到這裡
    return v > 1.0 ? 1.0 : v;
}

struct Vec {
    double x{0.0};
    double y{0.0};
};

[[nodiscard]] Vec sub(const PointF& a, const PointF& b) noexcept { return Vec{a.x - b.x, a.y - b.y}; }
[[nodiscard]] double length(const Vec& v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }

[[nodiscard]] PointF offsetBy(const PointF& p, const Vec& dir, double distance) noexcept {
    return PointF{p.x + dir.x * distance, p.y + dir.y * distance};
}

// 累積實際塗到的範圍，作為 /BBox。
//
// 直接用呼叫端給的 /Rect 當 BBox 是常見捷徑，但線寬、箭頭與波浪振幅都會超出
// 幾何本身；BBox 一旦小於實際筆跡，Acrobat 會把超出的部分裁掉，而且只在
// 高倍率下看得出來。所以 BBox 由繪圖過程反推。
class BoundsAccumulator {
public:
    void add(const PointF& p) noexcept {
        if (empty_) {
            bounds_ = RectF{p.x, p.y, p.x, p.y};
            empty_ = false;
            return;
        }
        bounds_.left = std::min(bounds_.left, p.x);
        bounds_.right = std::max(bounds_.right, p.x);
        bounds_.bottom = std::min(bounds_.bottom, p.y);
        bounds_.top = std::max(bounds_.top, p.y);
    }

    void addRect(const RectF& r) noexcept {
        add(PointF{r.left, r.bottom});
        add(PointF{r.right, r.top});
    }

    void inflate(double amount) noexcept {
        if (empty_ || amount <= 0.0) return;
        bounds_.left -= amount;
        bounds_.bottom -= amount;
        bounds_.right += amount;
        bounds_.top += amount;
    }

    [[nodiscard]] bool isEmpty() const noexcept { return empty_; }
    [[nodiscard]] RectF result() const noexcept { return bounds_; }

private:
    RectF bounds_{};
    bool empty_{true};
};

class ContentWriter {
public:
    void save() { token("q"); }
    void restore() { token("Q"); }
    void applyExtGState(const std::string& name) { buffer_ += '/'; buffer_ += name; buffer_ += " gs\n"; }

    void setStrokeColor(const ColorRgb& c) {
        number(c.r);
        number(c.g);
        number(c.b);
        token("RG");
    }

    void setFillColor(const ColorRgb& c) {
        number(c.r);
        number(c.g);
        number(c.b);
        token("rg");
    }

    void setLineWidth(double w) {
        number(w);
        token("w");
    }

    // 手繪筆畫的端點與轉角必須是圓的，否則折線在急轉處會露出尖角缺口。
    void setRoundJoins() { token("1 J"); token("1 j"); }

    void moveTo(const PointF& p) { xy(p); token("m"); }
    void lineTo(const PointF& p) { xy(p); token("l"); }

    void curveTo(const PointF& c1, const PointF& c2, const PointF& end) {
        xy(c1);
        xy(c2);
        xy(end);
        token("c");
    }

    void closePath() { token("h"); }

    void appendRect(const RectF& r) {
        number(r.left);
        number(r.bottom);
        number(r.width());
        number(r.height());
        token("re");
    }

    void paint(const char* op) { token(op); }

    // 裁切：路徑必須先建好（例如 appendRect），呼叫這個把它變成裁切區
    // 而不畫出來——與 W 之後緊接 n 是 ISO 32000 唯一合法的「只裁切不繪製」寫法。
    void clipNoPaint() { token("W"); token("n"); }

    // cm：把後續繪製映射到指定的矩陣。自訂圖片圖章要用它把單位正方形的
    // 影像空間放到 /Rect 上——影像 XObject 的座標系永遠是 0..1 的正方形。
    void concatMatrix(double a, double b, double c, double d, double e, double f) {
        number(a);
        number(b);
        number(c);
        number(d);
        number(e);
        number(f);
        token("cm");
    }

    void drawXObject(const std::string& resourceName) {
        buffer_ += '/';
        buffer_ += resourceName;
        buffer_ += " Do\n";
    }

    void beginText() { token("BT"); }
    void endText() { token("ET"); }

    void setFont(const std::string& resourceName, double size) {
        buffer_ += '/';
        buffer_ += resourceName;
        buffer_ += ' ';
        buffer_ += formatNumber(size);
        buffer_ += " Tf\n";
    }

    // 相對於前一次文字位置的位移（PDF 的 Td 恆為相對量）。
    void textMoveTo(double dx, double dy) {
        number(dx);
        number(dy);
        token("Td");
    }

    // literalBytes 必須是已跳脫過的內容（見 escapeContentLiteral），
    // 這裡只負責補外層括號，不重複跳脫——雙重跳脫會讓括號配對失衡。
    void showText(const std::string& literalBytes) {
        buffer_ += '(';
        buffer_ += literalBytes;
        buffer_ += ") Tj\n";
    }

    [[nodiscard]] const std::string& str() const noexcept { return buffer_; }

private:
    void xy(const PointF& p) {
        number(p.x);
        number(p.y);
    }

    void number(double v) {
        buffer_ += formatNumber(v);
        buffer_ += ' ';
    }

    void token(const char* t) {
        buffer_ += t;
        buffer_ += '\n';
    }

    std::string buffer_;
};

// 依註解型別決定是否需要 /ExtGState，並在需要但容器不支援時記錄降級。
struct GraphicsState {
    bool needed{false};
    ExtGState state{};
};

[[nodiscard]] GraphicsState resolveGraphicsState(const Annotation& annotation) {
    const double alpha = clampUnit(annotation.opacity);
    // 螢光筆一律 Multiply：正常混合會把底下的文字整片蓋掉，那不叫螢光筆。
    const bool multiply = annotation.type() == AnnotationType::Highlight;
    GraphicsState result{};
    result.needed = alpha < 1.0 || multiply;
    result.state = ExtGState{"GS0", alpha, alpha, multiply ? BlendMode::Multiply : BlendMode::Normal};
    return result;
}

[[nodiscard]] double markupThickness(double lineHeight) noexcept {
    return std::max(lineHeight * kMarkupThicknessRatio, kMinMarkupThickness);
}

// 一組 quad 的區域框架：底邊與「向上」單位向量。
// 旋轉頁上的文字行 quad 不是軸對齊的，底線／刪除線必須沿著 quad 自己的方向走，
// 用頁面的 Y 軸去算會在旋轉頁上畫出斜的線。
struct QuadFrame {
    bool valid{false};
    PointF bottomLeft{};
    PointF bottomRight{};
    Vec up{};
    double height{0.0};
};

[[nodiscard]] QuadFrame frameOf(const QuadPoint& quad) noexcept {
    const Vec upVector = sub(quad.upperLeft, quad.lowerLeft);
    const double h = length(upVector);
    if (!(h > 0.0)) return QuadFrame{};
    return QuadFrame{true, quad.lowerLeft, quad.lowerRight, Vec{upVector.x / h, upVector.y / h}, h};
}

void writeHighlight(ContentWriter& writer, BoundsAccumulator& bounds,
                    const domain::TextMarkupGeometry& geometry, const ColorRgb& color,
                    std::size_t& pathCount) {
    writer.setFillColor(color);
    for (const QuadPoint& quad : geometry.quads) {
        const QuadFrame frame = frameOf(quad);
        if (!frame.valid) continue;
        // 依左上→右上→右下→左下的順序繞行，與 QuadPoints 的角序一致；
        // 繞錯順序會得到自交的蝴蝶結形狀，非零填充規則下中央會出現空洞。
        writer.moveTo(quad.upperLeft);
        writer.lineTo(quad.upperRight);
        writer.lineTo(quad.lowerRight);
        writer.lineTo(quad.lowerLeft);
        writer.closePath();
        bounds.add(quad.upperLeft);
        bounds.add(quad.upperRight);
        bounds.add(quad.lowerLeft);
        bounds.add(quad.lowerRight);
        ++pathCount;
    }
    if (pathCount > 0) writer.paint("f");
}

void writeStraightMarkup(ContentWriter& writer, BoundsAccumulator& bounds,
                         const domain::TextMarkupGeometry& geometry, const ColorRgb& color,
                         double offsetRatio, std::size_t& pathCount) {
    writer.setStrokeColor(color);
    double maxThickness = 0.0;
    std::string pending;
    for (const QuadPoint& quad : geometry.quads) {
        const QuadFrame frame = frameOf(quad);
        if (!frame.valid) continue;
        const double thickness = markupThickness(frame.height);
        maxThickness = std::max(maxThickness, thickness);
        const double distance = frame.height * offsetRatio;
        const PointF a = offsetBy(frame.bottomLeft, frame.up, distance);
        const PointF b = offsetBy(frame.bottomRight, frame.up, distance);
        if (pathCount == 0) writer.setLineWidth(thickness);
        writer.moveTo(a);
        writer.lineTo(b);
        bounds.add(a);
        bounds.add(b);
        ++pathCount;
    }
    if (pathCount > 0) {
        writer.paint("S");
        bounds.inflate(maxThickness * 0.5);
    }
}

void writeSquiggly(ContentWriter& writer, BoundsAccumulator& bounds,
                   const domain::TextMarkupGeometry& geometry, const ColorRgb& color,
                   std::size_t& pathCount) {
    writer.setStrokeColor(color);
    double maxThickness = 0.0;
    for (const QuadPoint& quad : geometry.quads) {
        const QuadFrame frame = frameOf(quad);
        if (!frame.valid) continue;
        const Vec along = sub(frame.bottomRight, frame.bottomLeft);
        const double span = length(along);
        if (!(span > 0.0)) continue;
        const Vec dir{along.x / span, along.y / span};
        const double amplitude = frame.height * kSquigglyAmplitudeRatio;
        const double period = std::max(frame.height * kSquigglyPeriodRatio, 1.0);
        const double thickness = markupThickness(frame.height);
        maxThickness = std::max(maxThickness, thickness);
        if (pathCount == 0) writer.setLineWidth(thickness);

        const PointF base = offsetBy(frame.bottomLeft, frame.up, amplitude);
        writer.moveTo(base);
        bounds.add(base);
        bool up = true;
        for (double travelled = period; travelled <= span; travelled += period) {
            const PointF onBase = offsetBy(base, dir, travelled);
            const PointF peak = offsetBy(onBase, frame.up, up ? amplitude : -amplitude);
            writer.lineTo(peak);
            bounds.add(peak);
            up = !up;
        }
        ++pathCount;
    }
    if (pathCount > 0) {
        writer.paint("S");
        bounds.inflate(maxThickness * 0.5);
    }
}

// 幾何註解的描邊要完全落在 /Rect 內，因此路徑往內縮半個線寬。
// 不縮的話 Acrobat 會把外側半條線裁掉，看起來像線變細了一半。
[[nodiscard]] RectF insetForStroke(const RectF& rect, double strokeWidth) noexcept {
    const double half = strokeWidth * 0.5;
    RectF inset{rect.left + half, rect.bottom + half, rect.right - half, rect.top - half};
    if (inset.width() <= 0.0) {
        const double cx = (rect.left + rect.right) * 0.5;
        inset.left = cx;
        inset.right = cx;
    }
    if (inset.height() <= 0.0) {
        const double cy = (rect.bottom + rect.top) * 0.5;
        inset.bottom = cy;
        inset.top = cy;
    }
    return inset;
}

[[nodiscard]] const char* paintOperator(bool stroke, bool fill) noexcept {
    if (stroke && fill) return "B";
    if (fill) return "f";
    if (stroke) return "S";
    return "n";
}

void writeEllipse(ContentWriter& writer, const RectF& rect) {
    const double cx = (rect.left + rect.right) * 0.5;
    const double cy = (rect.bottom + rect.top) * 0.5;
    const double rx = rect.width() * 0.5;
    const double ry = rect.height() * 0.5;
    const double ox = rx * kEllipseKappa;
    const double oy = ry * kEllipseKappa;

    writer.moveTo(PointF{cx + rx, cy});
    writer.curveTo(PointF{cx + rx, cy + oy}, PointF{cx + ox, cy + ry}, PointF{cx, cy + ry});
    writer.curveTo(PointF{cx - ox, cy + ry}, PointF{cx - rx, cy + oy}, PointF{cx - rx, cy});
    writer.curveTo(PointF{cx - rx, cy - oy}, PointF{cx - ox, cy - ry}, PointF{cx, cy - ry});
    writer.curveTo(PointF{cx + ox, cy - ry}, PointF{cx + rx, cy - oy}, PointF{cx + rx, cy});
    writer.closePath();
}

void writeRoundedRect(ContentWriter& writer, const RectF& rect, double radius) {
    const double r = std::min(radius, std::min(rect.width(), rect.height()) * 0.5);
    const double o = r * kEllipseKappa;
    writer.moveTo(PointF{rect.left + r, rect.bottom});
    writer.lineTo(PointF{rect.right - r, rect.bottom});
    writer.curveTo(PointF{rect.right - r + o, rect.bottom}, PointF{rect.right, rect.bottom + r - o},
                   PointF{rect.right, rect.bottom + r});
    writer.lineTo(PointF{rect.right, rect.top - r});
    writer.curveTo(PointF{rect.right, rect.top - r + o}, PointF{rect.right - r + o, rect.top},
                   PointF{rect.right - r, rect.top});
    writer.lineTo(PointF{rect.left + r, rect.top});
    writer.curveTo(PointF{rect.left + r - o, rect.top}, PointF{rect.left, rect.top - r + o},
                   PointF{rect.left, rect.top - r});
    writer.lineTo(PointF{rect.left, rect.bottom + r});
    writer.curveTo(PointF{rect.left, rect.bottom + r - o}, PointF{rect.left + r - o, rect.bottom},
                   PointF{rect.left + r, rect.bottom});
    writer.closePath();
}

// 箭頭端點。回傳是否真的畫了東西，讓呼叫端據以決定 BBox。
bool writeArrowHead(ContentWriter& writer, BoundsAccumulator& bounds, const PointF& tip,
                    const Vec& incoming, double strokeWidth, domain::LineEnding ending) {
    if (ending == domain::LineEnding::None) return false;
    const double len = std::max(strokeWidth * kArrowLengthRatio, kMinArrowLength);
    const PointF back = offsetBy(tip, incoming, -len);
    const Vec normal{-incoming.y, incoming.x};
    const double halfWidth = len * kArrowHalfWidthRatio;
    const PointF left = offsetBy(back, normal, halfWidth);
    const PointF right = offsetBy(back, normal, -halfWidth);

    if (ending == domain::LineEnding::ClosedArrow) {
        writer.moveTo(tip);
        writer.lineTo(left);
        writer.lineTo(right);
        writer.closePath();
        writer.paint("B");
    } else {
        writer.moveTo(left);
        writer.lineTo(tip);
        writer.lineTo(right);
        writer.paint("S");
    }
    bounds.add(left);
    bounds.add(right);
    bounds.add(tip);
    return true;
}

Appearance failure(std::string reason) {
    Appearance appearance{};
    appearance.valid = false;
    appearance.diagnostic = std::move(reason);
    return appearance;
}

// ISO 32000 §7.3.4.2 的常值字串跳脫，僅處理反斜線與括號。
//
// 這個工作重複了 engine/objects/pdf_object.h 的 escapeLiteralString，是分層
// 邊界逼出來的：alioth_annotations 是 alioth_objects 的上游相依（objects 連
// annotations，不是反過來），這裡不能連回 objects 去借那個函式，否則會是
// 循環相依。輸入已由 text_layout::layoutText 限制在可列印 ASCII，
// 因此只需要處理這三個字元，不是整份縮小版剖析器。
[[nodiscard]] std::string escapeContentLiteral(const std::string& ascii) {
    std::string out;
    out.reserve(ascii.size());
    for (const char c : ascii) {
        if (c == '(' || c == ')' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// 依對齊方式算出每一行文字的起始 x（框內座標系，原點在 /Rect 左下）。
[[nodiscard]] double freeTextLineX(domain::TextAlign align, const std::string& line,
                                   double fontSize, double innerWidth) {
    const double lineWidth = estimateTextWidth(line, fontSize);
    switch (align) {
        case domain::TextAlign::Center:
            return std::max(0.0, (innerWidth - lineWidth) / 2.0);
        case domain::TextAlign::Right:
            return std::max(0.0, innerWidth - lineWidth);
        case domain::TextAlign::Left:
            break;
    }
    return 0.0;
}

// Text Box／Typewriter／Callout 共用的繪製流程（WP24）。
//
// 三者的差異收斂成三個開關：要不要畫框（Typewriter 一律不畫，這是 PRD
// 「無邊框直接打字」的定義本身，不是使用者可調的邊框寬度為零而已）、
// 要不要畫引線（只有 Callout）、以及要不要參與量測比例（三者都不參與，
// FreeText 不是量測註解）。回傳值是失敗原因；空字串代表成功。
[[nodiscard]] std::string writeFreeText(ContentWriter& writer, BoundsAccumulator& bounds,
                                        const Annotation& annotation,
                                        const domain::FreeTextGeometry& text, double strokeWidth,
                                        bool hasStroke, bool& needsFont, bool& usedCjk,
                                        std::set<char32_t>& cjkUsed) {
    const RectF rect = annotation.rect.normalized();
    if (rect.isEmpty()) return "FreeText 註解的 Rect 是空的";

    bounds.addRect(rect);

    // Typewriter 明確定義成無邊框：即使呼叫端仍帶著非零的 border.width，
    // 這個型別的語意就是不畫框，不是「邊框剛好是零」的特例。
    const bool wantsChrome = text.intent != domain::FreeTextIntent::Typewriter;
    const bool hasFill = annotation.interiorColor.has_value();
    if (wantsChrome && (hasStroke || hasFill)) {
        if (hasStroke) {
            writer.setStrokeColor(annotation.color);
            writer.setLineWidth(strokeWidth);
        }
        if (hasFill) writer.setFillColor(*annotation.interiorColor);
        const RectF path = insetForStroke(rect, hasStroke ? strokeWidth : 0.0);
        writer.appendRect(path);
        writer.paint(paintOperator(hasStroke, hasFill));
    }

    if (text.intent == domain::FreeTextIntent::Callout && text.callout.has_value()) {
        const domain::CalloutLine& line = *text.callout;
        const double leaderWidth = hasStroke ? strokeWidth : 1.0;
        writer.setStrokeColor(annotation.color);
        writer.setLineWidth(leaderWidth);
        writer.moveTo(line.start);
        bounds.add(line.start);
        PointF beforeEnd = line.start;
        if (line.knee.has_value()) {
            writer.lineTo(*line.knee);
            bounds.add(*line.knee);
            beforeEnd = *line.knee;
        }
        writer.lineTo(line.end);
        bounds.add(line.end);
        writer.paint("S");

        const Vec incoming = [&] {
            const Vec d = sub(line.end, beforeEnd);
            const double len = length(d);
            return len > 0.0 ? Vec{d.x / len, d.y / len} : Vec{0.0, -1.0};
        }();
        writeArrowHead(writer, bounds, line.end, incoming, leaderWidth, line.ending);
    }

    const double innerWidth = std::max(0.0, rect.width() - kFreeTextPaddingPt * 2.0);
    const double innerHeight = std::max(0.0, rect.height() - kFreeTextPaddingPt * 2.0);

    // 縮排（PRD-ANN-031）：整段文字的可用寬度先讓出縮排量，換行與對齊都在
    // 縮小後的寬度裡計算——縮排量本身超過可用寬度時（使用者拖出一個很窄
    // 的框又設了很大的縮排）就讓可用寬度變 0，換行結果會是每行都清空，
    // 不強行給負寬度讓 layoutText 算出無意義的結果。
    const double indent = std::clamp(text.indentPt, 0.0, innerWidth);
    const double wrapWidth = std::max(0.0, innerWidth - indent);

    TextFitOptions fit{};
    fit.fontSize = text.fontSize > 0.0 ? text.fontSize : 12.0;
    fit.maxWidth = wrapWidth;
    // 行距（PRD-ANN-031）：<= 0 視為未設定，退回原本的預設值，
    // 不能讓一個沒填的欄位變成零高度或負向排版。
    fit.lineSpacingRatio = text.lineSpacing > 0.0 ? text.lineSpacing : 1.2;
    const TextLayoutResult layout = layoutText(text.text, fit);
    if (!layout.ok) return layout.diagnostic;

    const bool hasVisibleText =
        !layout.lines.empty() &&
        std::any_of(layout.lines.begin(), layout.lines.end(),
                   [](const std::string& l) { return !l.empty(); });
    if (hasVisibleText) {
        needsFont = true;
        writer.save();
        // 文字必須裁在框內，否則超長內容會畫出框外蓋住鄰近註解——
        // PRD-ANN-030「Fit Box by Text Content」正是用來避免要靠裁切收尾的情況，
        // 但呼叫端沒先貼合時，裁切是唯一防線。
        writer.appendRect(RectF{rect.left + kFreeTextPaddingPt, rect.bottom + kFreeTextPaddingPt,
                                rect.left + kFreeTextPaddingPt + innerWidth,
                                rect.bottom + kFreeTextPaddingPt + innerHeight});
        writer.clipNoPaint();

        writer.beginText();
        writer.setFillColor(text.textColor);
        double y = rect.top - kFreeTextPaddingPt - fit.fontSize;
        double previousX = 0.0;
        double previousY = rect.top - kFreeTextPaddingPt;
        bool first = true;
        for (const std::string& line : layout.lines) {
            const double x = freeTextLineX(text.align, line, fit.fontSize, wrapWidth);
            const double baseX = rect.left + kFreeTextPaddingPt + indent + x;
            if (first) {
                writer.textMoveTo(baseX, y);
                first = false;
            } else {
                writer.textMoveTo(baseX - previousX, y - previousY);
            }
            previousX = baseX;
            previousY = y;
            // 一行裡可能同時有拉丁與中文。兩者的字型不同，因此必須切成
            // 一段一段輪流切換字型畫出來——整行用同一個字型的話，不是中文
            // 變成亂碼，就是拉丁字被當成雙位元組的 GID 讀掉。
            for (const fonts::TextRun& run : fonts::splitTextRuns(line)) {
                if (run.cjk) {
                    writer.setFont("CJK", fit.fontSize);
                    writer.showText(run.bytes);
                    cjkUsed.insert(run.codepoints.begin(), run.codepoints.end());
                    usedCjk = true;
                } else {
                    writer.setFont("Helv", fit.fontSize);
                    writer.showText(escapeContentLiteral(run.bytes));
                }
            }
            y -= layout.lineHeight;
        }
        writer.endText();
        writer.restore();
    }

    return {};
}

// 校正符號（None：插入記號 ^；Paragraph：新段落記號 ¶ 的向量近似，見下方
// 函式註解）。純向量繪製、不需要字型——Caret 的兩種符號都是固定形狀，
// 用路徑畫比帶字型子集划算，也不受字型策略未定案影響。
void writeCaretInsertMark(ContentWriter& writer, BoundsAccumulator& bounds, const RectF& rect,
                          double strokeWidth, const ColorRgb& color) {
    const double cx = (rect.left + rect.right) * 0.5;
    writer.setStrokeColor(color);
    writer.setLineWidth(strokeWidth);
    writer.setRoundJoins();
    writer.moveTo(PointF{rect.left, rect.bottom});
    writer.lineTo(PointF{cx, rect.top});
    writer.lineTo(PointF{rect.right, rect.bottom});
    writer.paint("S");
    bounds.addRect(rect);
    bounds.inflate(strokeWidth * 0.5);
}

// ¶ 的向量近似：一個圓形的「碗」加一根矩形的「莖」。不是精確的字型輪廓，
// 是刻意的範圍控制（CLAUDE.md 待決策：CJK／符號字型內嵌策略未定案之前，
// 不為了畫一個符號去內嵌任何字型）。兩塊各自獨立填色，不做布林運算挖洞，
// 因此莖會整根塞滿碗的下半，視覺上足以辨識為 ¶，但不是排版級的還原。
void writeCaretParagraphMark(ContentWriter& writer, BoundsAccumulator& bounds, const RectF& rect,
                             const ColorRgb& color) {
    const double width = rect.width();
    writer.setFillColor(color);

    const double stemWidth = std::max(width * 0.16, 0.5);
    const double stemLeft = rect.left + width * 0.52;
    writer.appendRect(RectF{stemLeft, rect.bottom, stemLeft + stemWidth, rect.top});
    writer.paint("f");

    const double bowlRadius = width * 0.32;
    const double bowlCx = rect.left + width * 0.40;
    const double bowlCy = rect.top - bowlRadius;
    writeEllipse(writer, RectF{bowlCx - bowlRadius, bowlCy - bowlRadius, bowlCx + bowlRadius,
                               bowlCy + bowlRadius});
    writer.paint("f");

    bounds.addRect(rect);
}

// 便利貼圖示的 /Matrix：把頁面 /Rotate 反轉掉，讓圖示在旋轉檢視下仍然正立。
//
// BBox 保持正方形是這裡的前提——§12.5.5 會把「Matrix 變換後的 BBox 外接框」
// 對映回 /Rect，正方形繞中心旋轉 90 度後外接框不變，對映因此仍是恆等，
// 圖示不會被拉伸。非正方形的 BBox 走這條路會變形。
[[nodiscard]] std::array<double, 6> counterRotationMatrix(domain::Rotation rotation,
                                                          const RectF& bbox) noexcept {
    if (rotation == domain::Rotation::None) return {1, 0, 0, 1, 0, 0};
    const double radians = -domain::rotationDegrees(rotation) * 3.14159265358979323846 / 180.0;
    const double c = std::round(std::cos(radians));
    const double s = std::round(std::sin(radians));
    const double cx = (bbox.left + bbox.right) * 0.5;
    const double cy = (bbox.bottom + bbox.top) * 0.5;
    return {c, s, -s, c, cx - (c * cx - s * cy), cy - (s * cx + c * cy)};
}


// 沿著一條封閉路徑畫雲狀邊框（/BE /S /C，PRD-ANN-002 的雲線）。
//
// 規格只說「邊框畫成雲狀」，沒有規定弧的畫法，各家實作長得都不一樣。
// 這裡的作法是沿邊等距擺放半圓弧，凸向多邊形外側：
//
//   - 弧半徑由 /I 決定（intensity 0/1/2 → 規格允許的三檔起伏）。
//   - 每一段邊上的弧數取整，**至少一個**：邊比一個弧還短時仍要有雲的樣子，
//     否則短邊會退化成直線，整個形狀看起來像「有幾邊忘了畫」。
//   - 凸向外側靠的是頂點順序決定的法線方向。順序反過來時雲會凹進去——
//     那看起來像被咬過一口，是這個效果最容易做錯的地方，因此先算一次
//     有向面積決定要往哪邊凸。
void writeCloudyPath(ContentWriter& writer, const std::vector<PointF>& vertices,
                     double intensity, BoundsAccumulator& bounds) {
    if (vertices.size() < 3) return;

    // /I 只定義 0/1/2。夾住而不是原樣使用：超出範圍的值行為未定義。
    const double clamped = std::clamp(intensity, 0.0, 2.0);
    const double radius = 4.0 + clamped * 3.0;

    // 有向面積（鞋帶公式）決定頂點順序是順時針還是逆時針，
    // 用它挑出「向外」的法線方向。
    double signedArea = 0.0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const PointF& a = vertices[i];
        const PointF& b = vertices[(i + 1) % vertices.size()];
        signedArea += a.x * b.y - b.x * a.y;
    }
    const double outward = signedArea >= 0.0 ? -1.0 : 1.0;

    bool started = false;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const PointF& from = vertices[i];
        const PointF& to = vertices[(i + 1) % vertices.size()];
        const double dx = to.x - from.x;
        const double dy = to.y - from.y;
        const double length = std::hypot(dx, dy);
        if (length <= 0.0) continue;

        const int arcs = std::max(1, static_cast<int>(std::lround(length / (radius * 2.0))));
        const double step = length / arcs;
        const double ux = dx / length;
        const double uy = dy / length;
        // 法線：把單位向量轉 90 度，再依 outward 決定朝哪一側。
        const double nx = -uy * outward;
        const double ny = ux * outward;

        for (int arc = 0; arc < arcs; ++arc) {
            const PointF start{from.x + ux * step * arc, from.y + uy * step * arc};
            const PointF end{from.x + ux * step * (arc + 1), from.y + uy * step * (arc + 1)};
            if (!started) {
                writer.moveTo(start);
                started = true;
            }
            // 以三次貝茲近似半圓。控制點距離取 step 的 0.55 倍——
            // 圓的貝茲近似常數 4/3*(sqrt(2)-1) ≈ 0.5523。
            const double bulge = step * 0.55;
            const PointF c1{start.x + nx * bulge, start.y + ny * bulge};
            const PointF c2{end.x + nx * bulge, end.y + ny * bulge};
            writer.curveTo(c1, c2, end);
            bounds.add(start);
            bounds.add(end);
            // 弧凸出去的部分也要算進外框，否則 /Rect 會把雲的頂端切掉。
            bounds.add(PointF{(start.x + end.x) * 0.5 + nx * bulge,
                              (start.y + end.y) * 0.5 + ny * bulge});
        }
    }
    writer.closePath();
}

}  // namespace

std::string formatNumber(double value) {
    if (!std::isfinite(value)) value = 0.0;
    // PDF 的實數範圍實務上遠小於此，夾住是為了避免 long long 轉換溢位。
    constexpr double kLimit = 1.0e9;
    value = std::max(-kLimit, std::min(kLimit, value));

    const bool negative = value < 0.0;
    const double magnitude = negative ? -value : value;
    const long long scaled = static_cast<long long>(magnitude * 10000.0 + 0.5);
    const long long whole = scaled / 10000;
    const long long fraction = scaled % 10000;

    std::string out;
    if (negative && scaled != 0) out += '-';
    out += std::to_string(whole);
    if (fraction != 0) {
        std::string digits = std::to_string(fraction);
        digits.insert(digits.begin(), static_cast<std::size_t>(4) - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') digits.pop_back();
        out += '.';
        out += digits;
    }
    return out;
}

std::string Appearance::resourcesDictionary() const {
    if (extGStates.empty()) return "<< >>";
    std::string out = "<< /ExtGState << ";
    for (const ExtGState& state : extGStates) {
        out += '/';
        out += state.name;
        out += " << /Type /ExtGState /CA ";
        out += formatNumber(state.strokeAlpha);
        out += " /ca ";
        out += formatNumber(state.fillAlpha);
        out += " /BM /";
        out += (state.blend == BlendMode::Multiply ? "Multiply" : "Normal");
        out += " >> ";
    }
    out += ">> >>";
    return out;
}

Appearance generateAppearance(const Annotation& annotation, const AppearanceOptions& options) {
    const AnnotationType type = annotation.type();
    const GraphicsState graphics = resolveGraphicsState(annotation);

    ContentWriter writer;
    BoundsAccumulator bounds;

    writer.save();
    if (graphics.needed && options.resourcesSupported) writer.applyExtGState(graphics.state.name);

    const double strokeWidth = std::max(annotation.border.width, 0.0);
    const bool hasStroke = strokeWidth > 0.0;
    std::size_t pathCount = 0;
    std::string failureReason;
    bool freeTextNeedsFont = false;
    bool freeTextUsedCjk = false;
    std::set<char32_t> freeTextCjkCodepoints;
    bool stampNeedsFont = false;
    bool stampNeedsImage = false;

    if (const auto* markup = std::get_if<domain::TextMarkupGeometry>(&annotation.geometry)) {
        if (markup->quads.empty()) {
            failureReason = "文字標記註解沒有 QuadPoints";
        } else {
            switch (markup->kind) {
                case domain::TextMarkupKind::Highlight:
                    writeHighlight(writer, bounds, *markup, annotation.color, pathCount);
                    break;
                case domain::TextMarkupKind::Underline:
                    writeStraightMarkup(writer, bounds, *markup, annotation.color,
                                        kUnderlineOffsetRatio, pathCount);
                    break;
                case domain::TextMarkupKind::StrikeOut:
                    writeStraightMarkup(writer, bounds, *markup, annotation.color,
                                        kStrikeOutOffsetRatio, pathCount);
                    break;
                case domain::TextMarkupKind::Squiggly:
                    writeSquiggly(writer, bounds, *markup, annotation.color, pathCount);
                    break;
            }
            if (pathCount == 0) failureReason = "所有 QuadPoints 的高度都是零";
        }
    } else if (const auto* shape = std::get_if<domain::ShapeGeometry>(&annotation.geometry)) {
        const RectF rect = annotation.rect.normalized();
        const bool hasFill = annotation.interiorColor.has_value();
        if (rect.isEmpty()) {
            failureReason = "幾何註解的 Rect 是空的";
        } else if (!hasStroke && !hasFill) {
            failureReason = "幾何註解既無邊框寬度也無填色";
        } else {
            if (hasStroke) {
                writer.setStrokeColor(annotation.color);
                writer.setLineWidth(strokeWidth);
            }
            if (hasFill) writer.setFillColor(*annotation.interiorColor);
            const RectF path = insetForStroke(rect, hasStroke ? strokeWidth : 0.0);
            if (shape->kind == domain::ShapeKind::Square) {
                writer.appendRect(path);
            } else {
                writeEllipse(writer, path);
            }
            writer.paint(paintOperator(hasStroke, hasFill));
            bounds.addRect(rect);
            pathCount = 1;
        }
    } else if (const auto* line = std::get_if<domain::LineGeometry>(&annotation.geometry)) {
        const Vec delta = sub(line->end, line->start);
        const double span = length(delta);
        if (!(span > 0.0)) {
            failureReason = "直線註解的兩端點重合";
        } else if (!hasStroke) {
            failureReason = "直線註解的邊框寬度是零";
        } else {
            const Vec dir{delta.x / span, delta.y / span};
            writer.setStrokeColor(annotation.color);
            writer.setFillColor(annotation.color);
            writer.setLineWidth(strokeWidth);
            writer.moveTo(line->start);
            writer.lineTo(line->end);
            writer.paint("S");
            bounds.add(line->start);
            bounds.add(line->end);
            writeArrowHead(writer, bounds, line->end, dir, strokeWidth, line->endEnding);
            writeArrowHead(writer, bounds, line->start, Vec{-dir.x, -dir.y}, strokeWidth,
                           line->startEnding);
            bounds.inflate(strokeWidth * 0.5);
            pathCount = 1;
        }
    } else if (const auto* ink = std::get_if<domain::InkGeometry>(&annotation.geometry)) {
        if (!hasStroke) {
            failureReason = "手繪註解的筆畫寬度是零";
        } else {
            writer.setStrokeColor(annotation.color);
            writer.setLineWidth(strokeWidth);
            writer.setRoundJoins();
            for (const auto& stroke : ink->strokes) {
                if (stroke.empty()) continue;
                writer.moveTo(stroke.front());
                bounds.add(stroke.front());
                for (std::size_t i = 1; i < stroke.size(); ++i) {
                    writer.lineTo(stroke[i]);
                    bounds.add(stroke[i]);
                }
                // 單點筆畫要退化成一個圓點，否則點一下畫布會什麼都沒有。
                if (stroke.size() == 1) writer.lineTo(stroke.front());
                ++pathCount;
            }
            if (pathCount == 0) {
                failureReason = "手繪註解沒有任何筆畫";
            } else {
                writer.paint("S");
                bounds.inflate(strokeWidth * 0.5);
            }
        }
    } else if (const auto* polygon = std::get_if<domain::PolygonGeometry>(&annotation.geometry)) {
        const bool hasFill = annotation.interiorColor.has_value();
        if (polygon->vertices.size() < 3) {
            failureReason = "多邊形註解至少要三個頂點";
        } else if (!hasStroke && !hasFill) {
            failureReason = "多邊形註解既無邊框寬度也無填色";
        } else {
            if (hasStroke) {
                writer.setStrokeColor(annotation.color);
                writer.setLineWidth(strokeWidth);
                writer.setRoundJoins();
            }
            if (hasFill) writer.setFillColor(*annotation.interiorColor);
            if (polygon->borderEffect.isCloudy()) {
                // 雲線（/BE /S /C）。走同一條 /Polygon 路徑，只是邊界畫成雲狀。
                writeCloudyPath(writer, polygon->vertices, polygon->borderEffect.intensity,
                                bounds);
            } else {
                writer.moveTo(polygon->vertices.front());
                bounds.add(polygon->vertices.front());
                for (std::size_t i = 1; i < polygon->vertices.size(); ++i) {
                    writer.lineTo(polygon->vertices[i]);
                    bounds.add(polygon->vertices[i]);
                }
                // /Polygon 恆為封閉路徑（ISO 32000-1 §12.5.6.13），因此一律 closePath，
                // 不管使用者畫的最後一點是否等於起點。
                writer.closePath();
            }
            writer.paint(paintOperator(hasStroke, hasFill));
            if (hasStroke) bounds.inflate(strokeWidth * 0.5);
            pathCount = 1;
        }
    } else if (const auto* polyline = std::get_if<domain::PolyLineGeometry>(&annotation.geometry)) {
        if (polyline->vertices.size() < 2) {
            failureReason = "折線註解至少要兩個頂點";
        } else if (!hasStroke) {
            failureReason = "折線註解的邊框寬度是零";
        } else {
            writer.setStrokeColor(annotation.color);
            writer.setFillColor(annotation.color);
            writer.setLineWidth(strokeWidth);
            writer.setRoundJoins();
            writer.moveTo(polyline->vertices.front());
            bounds.add(polyline->vertices.front());
            for (std::size_t i = 1; i < polyline->vertices.size(); ++i) {
                writer.lineTo(polyline->vertices[i]);
                bounds.add(polyline->vertices[i]);
            }
            writer.paint("S");
            // 線端樣式套用在頭尾兩個頂點，方向取最後一段／第一段的走向，
            // 與 /Line 的箭頭邏輯一致（那裡只有一段，這裡是多段折線的頭尾段）。
            const std::size_t last = polyline->vertices.size() - 1;
            const Vec endDir = [&] {
                const Vec d = sub(polyline->vertices[last], polyline->vertices[last - 1]);
                const double len = length(d);
                return len > 0.0 ? Vec{d.x / len, d.y / len} : Vec{1.0, 0.0};
            }();
            const Vec startDir = [&] {
                const Vec d = sub(polyline->vertices[0], polyline->vertices[1]);
                const double len = length(d);
                return len > 0.0 ? Vec{d.x / len, d.y / len} : Vec{-1.0, 0.0};
            }();
            writeArrowHead(writer, bounds, polyline->vertices[last], endDir, strokeWidth,
                           polyline->endEnding);
            writeArrowHead(writer, bounds, polyline->vertices[0], startDir, strokeWidth,
                           polyline->startEnding);
            bounds.inflate(strokeWidth * 0.5);
            pathCount = 1;
        }
    } else if (const auto* caret = std::get_if<domain::CaretGeometry>(&annotation.geometry)) {
        const RectF rect = annotation.rect.normalized();
        if (rect.isEmpty()) {
            failureReason = "校正符號註解的 Rect 是空的";
        } else {
            const double caretStroke = hasStroke ? strokeWidth : std::max(1.0, rect.width() * 0.08);
            if (caret->symbol == domain::CaretSymbol::Paragraph) {
                writeCaretParagraphMark(writer, bounds, rect, annotation.color);
            } else {
                writeCaretInsertMark(writer, bounds, rect, caretStroke, annotation.color);
            }
            pathCount = 1;
        }
    } else if (const auto* freeText = std::get_if<domain::FreeTextGeometry>(&annotation.geometry)) {
        bool needsFontForText = false;
        bool usedCjkForText = false;
        std::set<char32_t> cjkForText;
        const std::string problem =
            writeFreeText(writer, bounds, annotation, *freeText, strokeWidth, hasStroke,
                         needsFontForText, usedCjkForText, cjkForText);
        if (!problem.empty()) {
            failureReason = problem;
        } else {
            pathCount = 1;
            freeTextNeedsFont = needsFontForText;
            freeTextUsedCjk = usedCjkForText;
            freeTextCjkCodepoints = std::move(cjkForText);
        }
    } else if (const auto* stamp = std::get_if<domain::StampGeometry>(&annotation.geometry)) {
        // 圖章的外觀一定要自己畫：PDFium 不會替 /Stamp 產生 /AP，而缺 /AP 的圖章
        // 在多數檢視器上是一片空白——不是錯誤訊息，就只是空白。
        const RectF rect = annotation.rect.normalized();
        if (rect.isEmpty()) {
            failureReason = "圖章的 Rect 是空的";
        } else if (stamp->isCustom()) {
            if (!stamp->hasImage()) {
                // 尺寸與位元組數不符的影像，畫出來會是斜的或直接踩到界外。
                failureReason = "自訂圖章沒有有效的影像資料";
            } else {
                // 影像空間是單位正方形，用 cm 把它映到 /Rect。
                writer.save();
                writer.concatMatrix(rect.width(), 0.0, 0.0, rect.height(), rect.left, rect.bottom);
                writer.drawXObject("Im0");
                writer.restore();
                bounds.addRect(rect);
                pathCount = 1;
                stampNeedsImage = true;
            }
        } else {
            const std::string label = domain::stampLabelOf(stamp->kind);
            const ColorRgb color = annotation.color;
            const double border = hasStroke ? strokeWidth : std::max(1.0, rect.height() * 0.06);

            writer.setStrokeColor(color);
            writer.setLineWidth(border);
            writeRoundedRect(writer, insetForStroke(rect, border), rect.height() * 0.18);
            writer.paint("S");
            bounds.addRect(rect);

            // 字級由框寬決定：圖章被縮小時字要跟著縮，否則字會滿出邊框。
            // 先以框高的一半試算，再依實際文字寬度收斂——量一次就夠，
            // 因為 Helvetica 的寬度與字級成正比。
            const double inner = rect.width() - border * 4.0;
            double fontSize = rect.height() * 0.45;
            const double naturalWidth = estimateTextWidth(label, fontSize);
            if (naturalWidth > inner && naturalWidth > 0.0) {
                fontSize *= inner / naturalWidth;
            }
            if (fontSize > 1.0) {
                const double textWidth = estimateTextWidth(label, fontSize);
                const double x = rect.left + (rect.width() - textWidth) / 2.0;
                // 基線放在垂直中線下方 0.35 個字高處，視覺上才是置中——
                // 字的重心在基線之上，照幾何中線放會看起來偏高。
                const double y = rect.bottom + rect.height() / 2.0 - fontSize * 0.35;
                writer.setFillColor(color);
                writer.beginText();
                writer.setFont("Helv", fontSize);
                writer.textMoveTo(x, y);
                writer.showText(escapeContentLiteral(label));
                writer.endText();
                stampNeedsFont = true;
            }
            pathCount = 1;
        }
    } else if (std::holds_alternative<domain::TextNoteGeometry>(annotation.geometry)) {
        // 目前所有 TextNoteIcon 共用同一個便條圖示。分別繪製 Comment / Help / Key
        // 等圖示屬於 WBS 4.7，那一包還會帶進彈出視窗，一起做才不會畫兩次。
        const RectF rect = annotation.rect.normalized();
        const double side = rect.isEmpty() ? kDefaultNoteSize
                                           : std::min(rect.width(), rect.height());
        // 便利貼的錨點是 /Rect 左上角，與 Acrobat 一致；往下長出一個正方形。
        const RectF icon{rect.left, rect.top - side, rect.left + side, rect.top};
        writer.setFillColor(annotation.color);
        writer.setStrokeColor(ColorRgb{annotation.color.r * 0.4, annotation.color.g * 0.4,
                                       annotation.color.b * 0.4});
        writer.setLineWidth(std::max(strokeWidth, side * 0.04));
        writeRoundedRect(writer, insetForStroke(icon, side * 0.08), side * 0.2);
        writer.paint("B");

        writer.setStrokeColor(ColorRgb{0.2, 0.2, 0.2});
        writer.setLineWidth(side * 0.05);
        for (int i = 1; i <= 3; ++i) {
            const double y = icon.bottom + side * (0.25 * static_cast<double>(i));
            writer.moveTo(PointF{icon.left + side * 0.22, y});
            writer.lineTo(PointF{icon.right - side * 0.22, y});
        }
        writer.paint("S");
        bounds.addRect(icon);
        pathCount = 4;
    }

    if (!failureReason.empty()) return failure(std::move(failureReason));
    if (bounds.isEmpty()) return failure("外觀串流沒有產生任何幾何");

    writer.restore();

    Appearance appearance{};
    appearance.valid = true;
    appearance.content = writer.str();
    appearance.bbox = bounds.result();
    appearance.needsFont = freeTextNeedsFont || stampNeedsFont;
    // 這兩者必須成對送出去：宣告用了 CJK 卻沒給碼點，寫入層做不出子集，
    // 中文就整段消失而且沒有錯誤訊息。
    appearance.needsCjkFont = freeTextUsedCjk;
    appearance.cjkCodepoints = std::move(freeTextCjkCodepoints);
    appearance.needsStampImage = stampNeedsImage;
    if (graphics.needed) {
        if (options.resourcesSupported) {
            appearance.extGStates.push_back(graphics.state);
        } else {
            appearance.resourcesElided = true;
        }
    }
    if (type == AnnotationType::Text) {
        appearance.matrix = counterRotationMatrix(options.pageRotation, appearance.bbox);
    }
    return appearance;
}

}  // namespace alioth::engine::annotations
