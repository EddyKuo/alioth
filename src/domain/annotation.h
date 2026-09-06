#pragma once

// 註解領域模型（PRD §6.3 ANN、WBS 4.1）。
//
// 純 C++：不得引入 Qt 或 PDFium。註解要能在無 GUI、無引擎的環境下被建構、
// 比較與序列化，外觀串流產生器（引擎轉接層）才可能單獨測試——那是全案
// 最高風險節點，把它的輸入端隔離在純資料上是刻意的設計。
//
// 所有座標一律為「未旋轉的頁面預設使用者空間」（點，原點左下、Y 向上）。
// 頁面 /Rotate 不改變註解座標系，因此本檔不出現任何旋轉處理；
// 由檢視座標換算而來的輸入請先經 PageTransform::toPage 或本檔的
// quadFromDeviceRect 轉換，禁止在呼叫端自行翻轉 Y 軸。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "domain/geometry.h"
#include "domain/measurement.h"
#include "domain/quad_point.h"

namespace alioth::domain {

// /C 與 /IC 的顏色成分，值域 0–1 的 DeviceRGB。
// PDF 的 /C 也允許 0/1/4 個成分（透明/灰階/CMYK），但審閱工具全程用 RGB，
// 在領域層就收斂掉可以省下下游每一處的分支。
struct ColorRgb {
    double r{0.0};
    double g{0.0};
    double b{0.0};

    friend constexpr bool operator==(const ColorRgb&, const ColorRgb&) = default;
};

// /BS /S
enum class BorderStyleKind : std::uint8_t {
    Solid,
    Dashed,
    Beveled,
    Inset,
    Underline,
};

struct BorderStyle {
    double width{1.0};
    BorderStyleKind style{BorderStyleKind::Solid};
    std::vector<double> dashPattern{};  // 僅 Dashed 有意義，對應 /BS /D

    friend bool operator==(const BorderStyle&, const BorderStyle&) = default;
};

// /F 註解旗標（ISO 32000-2 表 167）。PRD-ANN-012 的鎖定／隱藏／列印直接對應這裡。
enum class AnnotationFlag : std::uint32_t {
    None = 0,
    Invisible = 1u << 0,
    Hidden = 1u << 1,
    Print = 1u << 2,
    NoZoom = 1u << 3,
    NoRotate = 1u << 4,
    NoView = 1u << 5,
    ReadOnly = 1u << 6,
    Locked = 1u << 7,
    ToggleNoView = 1u << 8,
    LockedContents = 1u << 9,
};

[[nodiscard]] constexpr AnnotationFlag operator|(AnnotationFlag a, AnnotationFlag b) noexcept {
    return static_cast<AnnotationFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr AnnotationFlag operator&(AnnotationFlag a, AnnotationFlag b) noexcept {
    return static_cast<AnnotationFlag>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

constexpr AnnotationFlag& operator|=(AnnotationFlag& a, AnnotationFlag b) noexcept {
    a = a | b;
    return a;
}

[[nodiscard]] constexpr bool hasFlag(AnnotationFlag set, AnnotationFlag flag) noexcept {
    return (set & flag) != AnnotationFlag::None;
}

// /M 與 /CreationDate。
//
// 刻意不用 std::chrono::system_clock：PDF 日期字串需要本地時區偏移，
// C++20 的 tz 資料庫在 MSVC 之外的工具鏈支援度不一致，而註解只需要「把使用者
// 當下的時間原樣記下來」。轉換責任留在應用層，領域層只負責格式正確。
struct PdfDate {
    int year{0};
    int month{0};   // 1–12
    int day{0};     // 1–31
    int hour{0};
    int minute{0};
    int second{0};
    int tzHours{0};    // UTC 偏移小時，可為負
    int tzMinutes{0};  // UTC 偏移分鐘，恆為非負

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return year > 0 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
    }

    friend constexpr bool operator==(const PdfDate&, const PdfDate&) = default;
};

namespace detail {

inline void appendPadded(std::string& out, int value, int digits) {
    std::string text = std::to_string(value < 0 ? -value : value);
    while (static_cast<int>(text.size()) < digits) text.insert(text.begin(), '0');
    out += text;
}

}  // namespace detail

// 產生 ISO 32000-2 §7.9.4 的日期字串：D:YYYYMMDDHHmmSSOHH'mm'。
// 日期無效時回傳空字串，讓上層決定是否略過該鍵，而不是寫出一個壞值。
[[nodiscard]] inline std::string toPdfDateString(const PdfDate& date) {
    if (!date.isValid()) return {};
    std::string out = "D:";
    detail::appendPadded(out, date.year, 4);
    detail::appendPadded(out, date.month, 2);
    detail::appendPadded(out, date.day, 2);
    detail::appendPadded(out, date.hour, 2);
    detail::appendPadded(out, date.minute, 2);
    detail::appendPadded(out, date.second, 2);
    out += (date.tzHours < 0 ? '-' : '+');
    detail::appendPadded(out, date.tzHours, 2);
    out += '\'';
    detail::appendPadded(out, date.tzMinutes, 2);
    out += '\'';
    return out;
}

// 反向：解析 ISO 32000-2 §7.9.4 的日期字串。
//
// 只取到秒，時區部分刻意忽略——匯入場景（XFDF / FDF）只需要顯示日期，
// 而現實中的檔案在時區欄位上的寫法差異最大（有的省略、有的用 Z、有的
// 少了收尾的單引號）。硬要解析時區，換來的是把一堆合法檔案判成壞掉。
//
// 格式不符時回傳空日期（isValid() 為 false），呼叫端不該因此中止匯入：
// 日期是附加資訊，不是結構完整性的一部分。
[[nodiscard]] inline PdfDate fromPdfDateString(std::string_view text) {
    PdfDate date{};
    if (text.size() >= 2 && text[0] == 'D' && text[1] == ':') text.remove_prefix(2);
    if (text.size() < 14) return {};
    const auto field = [&](std::size_t pos, std::size_t len, bool& ok) {
        int value = 0;
        for (std::size_t i = pos; i < pos + len; ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                ok = false;
                return 0;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    };
    bool ok = true;
    date.year = field(0, 4, ok);
    date.month = field(4, 2, ok);
    date.day = field(6, 2, ok);
    date.hour = field(8, 2, ok);
    date.minute = field(10, 2, ok);
    date.second = field(12, 2, ok);
    if (!ok) return {};
    return date;
}

enum class TextMarkupKind : std::uint8_t {
    Highlight,
    Underline,
    StrikeOut,
    Squiggly,
};

enum class ShapeKind : std::uint8_t {
    Square,
    Circle,
};

enum class LineEnding : std::uint8_t {
    None,
    OpenArrow,
    ClosedArrow,
};

enum class TextNoteIcon : std::uint8_t {
    Note,
    Comment,
    Help,
    Key,
    Paragraph,
    NewParagraph,
    Insert,
};

// /Sy（Caret 的符號，ISO 32000-2 表 174）。None 是校對常見的插入符號（^），
// Paragraph 是新段落記號（¶）。兩者都不需要內嵌字型，純向量畫出。
enum class CaretSymbol : std::uint8_t {
    None,
    Paragraph,
};

// FreeText 家族的三種工具共用同一個 /Subtype，差別只在 /IT 與是否有邊框／引線
// （WP24：PRD-ANN-005 Text Box、PRD-ANN-021 Typewriter、PRD-ANN-022 Callout）。
// 用同一個 geometry 型別而不是三個，是因為它們的欄位完全重疊，分三個型別
// 只會讓 writeGeometryKeys 多出三份幾乎一樣的程式碼。
enum class FreeTextIntent : std::uint8_t {
    TextBox,     // /IT /FreeText，有邊框與可選填色
    Typewriter,  // /IT /FreeTextTypewriter，PRD 明定「無邊框直接打字」，強制不畫框
    Callout,     // /IT /FreeTextCallout，額外帶 /CL 引線
};

enum class TextAlign : std::uint8_t {
    Left,
    Center,
    Right,
};

// /CL：引線的起點（貼在文字框邊上）、可選折點、終點（指向的目標，箭頭畫在這裡）。
struct CalloutLine {
    PointF start{};
    std::optional<PointF> knee{};
    PointF end{};
    LineEnding ending{LineEnding::OpenArrow};

    friend bool operator==(const CalloutLine&, const CalloutLine&) = default;
};

// 文字標記類（/Highlight /Underline /StrikeOut /Squiggly）：形狀完全由 /QuadPoints 決定，
// 一行文字一組 quad。/Rect 是所有 quad 的外接矩形，由產生器算出而非呼叫端指定。
struct TextMarkupGeometry {
    TextMarkupKind kind{TextMarkupKind::Highlight};
    std::vector<QuadPoint> quads{};
};

// 幾何類（/Square /Circle）：形狀由 Annotation::rect 決定。
struct ShapeGeometry {
    ShapeKind kind{ShapeKind::Square};
};

// /Line：/L 的兩端點與 /LE 的線端樣式。
struct LineGeometry {
    PointF start{};
    PointF end{};
    LineEnding startEnding{LineEnding::None};
    LineEnding endEnding{LineEnding::None};
};

// /Ink：/InkList，每條筆畫是一串點。
struct InkGeometry {
    std::vector<std::vector<PointF>> strokes{};
};

// /Text 便利貼：只有圖示與展開狀態，尺寸慣例為固定的小方框。
struct TextNoteGeometry {
    TextNoteIcon icon{TextNoteIcon::Note};
    bool open{false};
};

// /Caret：校正符號，只有符號種類。位置與大小交給 Annotation::rect。
struct CaretGeometry {
    CaretSymbol symbol{CaretSymbol::None};
};

// /Stamp 的內建圖章種類（PRD-ANN-006）。
//
// 名稱直接對應 PDF 規格的標準 /Name 值，因為那些是別的檢視器認得的字串；
// 自己另創一套名字會讓圖章在 Acrobat 裡變成空白方框。
// 規格的標準集合比使用者期待的少很多（沒有「已付款」「機密」這類），
// 缺的以自訂圖片圖章補，不要偷偷把自訂名稱寫進 /Name。
enum class StampKind : std::uint8_t {
    Approved,
    Experimental,
    NotApproved,
    AsIs,
    Expired,
    Draft,
    Final,
    Confidential,
    ForComment,
    TopSecret,
    ForPublicRelease,
    NotForPublicRelease,
    Sold,
    Departmental,
    Custom,  // 走 /AP 內嵌影像，/Name 不寫
};

// /Stamp：圖章。位置與大小由 Annotation::rect 決定。
//
// 兩種來源，處置完全不同：
//   內建 — 寫 /Name，外觀由我們自己畫（PDFium 不會替 /Stamp 產生 /AP，
//          而缺 /AP 的圖章在多數檢視器上是空白的）
//   自訂 — 影像內嵌成 XObject 並在 /AP 的 /Resources 註冊
struct StampGeometry {
    StampKind kind{StampKind::Approved};

    // Custom 專用。RGB 或 RGBA 的原始像素，逐列由上而下、每列 width*channels 位元組。
    // 刻意不接受檔案路徑：路徑會在使用者移走檔案後變成空白圖章，
    // 而那時他早就忘了那個圖章是哪張圖。
    std::vector<std::uint8_t> imagePixels;
    std::int32_t imageWidth{0};
    std::int32_t imageHeight{0};
    std::int32_t imageChannels{3};  // 3 = RGB、4 = RGBA（alpha 走 /SMask）

    [[nodiscard]] bool isCustom() const noexcept { return kind == StampKind::Custom; }
    [[nodiscard]] bool hasImage() const noexcept {
        return imageWidth > 0 && imageHeight > 0 && (imageChannels == 3 || imageChannels == 4) &&
               imagePixels.size() == static_cast<std::size_t>(imageWidth) *
                                         static_cast<std::size_t>(imageHeight) *
                                         static_cast<std::size_t>(imageChannels);
    }
};

// /FreeText：Text Box／Typewriter／Callout 共用（WP24）。
//
// 文字內容目前僅能表示 WinAnsi/Helvetica 畫得出來的子集（可列印 ASCII 加換行）。
// CJK 字型內嵌授權策略未定案（CLAUDE.md 待決策清單），外觀產生器對非 ASCII
// 內容一律明確失敗，不輸出殘缺或亂碼的位元組——與 engine/objects/
// content_stream_appender.cpp 的既有立場一致。
struct FreeTextGeometry {
    std::string text{};
    double fontSize{12.0};
    ColorRgb textColor{0.0, 0.0, 0.0};
    TextAlign align{TextAlign::Left};
    FreeTextIntent intent{FreeTextIntent::TextBox};
    std::optional<CalloutLine> callout{};  // 僅 intent == Callout 時應有值

    // 段落屬性（PRD-ANN-031）。兩者都只影響外觀串流怎麼畫，沒有對應的標準
    // PDF 字典鍵可寫——ISO 32000 的 /FreeText 只有 /Q（對齊，已經在
    // annotation_object_writer 寫出）與 /DA，沒有行距、縮排的欄位；Acrobat
    // 自己也是烘進 /RC、/DS 這類非強制的私有格式。因此這兩個屬性與其餘
    // 版面設定一樣，只保證「我們畫出來的 /AP 對」，不保證能被 Acrobat
    // 讀回來重新編輯——那本來就超出「不修改原始內容串流、不破壞相容性」
    // 的承諾範圍。
    //
    // lineSpacing：行距係數，實際行高＝fontSize × lineSpacing
    // （與 engine::annotations::TextFitOptions::lineSpacingRatio 同一件事，
    // 只是這裡是使用者可調整、可存檔復原的那一份）。<= 0 視為未設定，
    // 外觀產生器會退回 1.2 的預設值，不會產生零高度或負向排版。
    double lineSpacing{1.2};

    // indentPt：整個段落的左縮排（點），對齊仍在縮排後的可用寬度內計算，
    // 因此置中/靠右對齊搭配縮排時，效果是「先讓出左邊一塊空間，再置中/
    // 靠右」，與 Word 的段落縮排是同一種語意。
    double indentPt{0.0};
};

// /BE 邊框效果（ISO 32000-1 表 167）。雲線（PRD-ANN-002 的「雲線」）就是
// /BE << /S /C /I intensity >>。
//
// 只支援 /S /C（雲狀）與省略（一般直線邊框）兩種。規格裡的 /S /S（無效果）
// 與省略同義，不另設一個值——兩個值代表同一件事，遲早會有人只處理其中一個。
//
// intensity 是 0、1、2（規格允許的值）。0 等於沒有效果，因此 kNone 就用 0 表示。
enum class BorderEffect : std::uint8_t {
    None,
    Cloudy,
};

struct BorderEffectSettings {
    BorderEffect effect{BorderEffect::None};
    // /I：雲的「起伏程度」。規格只定義 0/1/2，其他值的行為未定義，
    // 因此產生器會夾在這個範圍內而不是原樣寫出去。
    double intensity{1.0};

    [[nodiscard]] bool isCloudy() const noexcept {
        return effect == BorderEffect::Cloudy && intensity > 0.0;
    }

    friend bool operator==(const BorderEffectSettings&, const BorderEffectSettings&) = default;
};

// /Polygon：/Vertices 依序連接並自動封閉。量測用途是 PRD-ANN-014 的面積
// （PRD-ANN-026「多邊形→面積」），因此至少要三個頂點才構成合法幾何，
// 但這裡不做驗證——非法狀態留給外觀產生器與量測計算各自明確拒絕，
// 領域模型只描述資料形狀。
struct PolygonGeometry {
    std::vector<PointF> vertices{};
    // 雲線是多邊形加上 /BE，不是另一種註解型別——這是 PDF 規格的表達方式，
    // 另建一個 CloudGeometry 會讓「雲線是不是多邊形」在量測與編輯路徑上
    // 各自有一套答案。
    BorderEffectSettings borderEffect{};
};

// /PolyLine：/Vertices 依序連接、不封閉。量測用途是 PRD-ANN-014 的周長
// （PRD-ANN-026「折線→周長」，實際量到的是路徑總長，見 domain/measurement.h
// polylineLength 的說明）。/LE 與 /Line 一樣分別套用在起點與終點。
struct PolyLineGeometry {
    std::vector<PointF> vertices{};
    LineEnding startEnding{LineEnding::None};
    LineEnding endEnding{LineEnding::None};
};

// 幾何資料以 variant 承載，而型別由 variant 決定。
//
// 另一種寫法是「型別 enum + 共用欄位」，但那允許 type=Highlight 卻帶著 InkList
// 的非法狀態存在；用 variant 讓非法組合無法被建構出來，產生器也就不必為
// 「型別與資料不一致」寫防禦性分支。
using AnnotationGeometry =
    std::variant<TextMarkupGeometry, ShapeGeometry, LineGeometry, InkGeometry, TextNoteGeometry,
                CaretGeometry, FreeTextGeometry, PolygonGeometry, PolyLineGeometry,
                StampGeometry>;

// /Subtype。由 geometry 推導，不獨立儲存，避免兩份真相。
enum class AnnotationType : std::uint8_t {
    Highlight,
    Underline,
    StrikeOut,
    Squiggly,
    Square,
    Circle,
    Line,
    Ink,
    Text,
    Caret,
    FreeText,
    Polygon,
    PolyLine,
    Stamp,
};

[[nodiscard]] inline AnnotationType typeOf(const AnnotationGeometry& geometry) noexcept {
    struct Visitor {
        AnnotationType operator()(const TextMarkupGeometry& g) const noexcept {
            switch (g.kind) {
                case TextMarkupKind::Highlight: return AnnotationType::Highlight;
                case TextMarkupKind::Underline: return AnnotationType::Underline;
                case TextMarkupKind::StrikeOut: return AnnotationType::StrikeOut;
                case TextMarkupKind::Squiggly:  return AnnotationType::Squiggly;
            }
            return AnnotationType::Highlight;
        }
        AnnotationType operator()(const ShapeGeometry& g) const noexcept {
            return g.kind == ShapeKind::Square ? AnnotationType::Square : AnnotationType::Circle;
        }
        AnnotationType operator()(const LineGeometry&) const noexcept { return AnnotationType::Line; }
        AnnotationType operator()(const InkGeometry&) const noexcept { return AnnotationType::Ink; }
        AnnotationType operator()(const TextNoteGeometry&) const noexcept { return AnnotationType::Text; }
        AnnotationType operator()(const CaretGeometry&) const noexcept { return AnnotationType::Caret; }
        AnnotationType operator()(const FreeTextGeometry&) const noexcept { return AnnotationType::FreeText; }
        AnnotationType operator()(const PolygonGeometry&) const noexcept { return AnnotationType::Polygon; }
        AnnotationType operator()(const PolyLineGeometry&) const noexcept { return AnnotationType::PolyLine; }
        AnnotationType operator()(const StampGeometry&) const noexcept { return AnnotationType::Stamp; }
    };
    return std::visit(Visitor{}, geometry);
}

[[nodiscard]] constexpr const char* subtypeName(AnnotationType type) noexcept {
    switch (type) {
        case AnnotationType::Highlight: return "Highlight";
        case AnnotationType::Underline: return "Underline";
        case AnnotationType::StrikeOut: return "StrikeOut";
        case AnnotationType::Squiggly:  return "Squiggly";
        case AnnotationType::Square:    return "Square";
        case AnnotationType::Circle:    return "Circle";
        case AnnotationType::Line:      return "Line";
        case AnnotationType::Ink:       return "Ink";
        case AnnotationType::Text:      return "Text";
        case AnnotationType::Caret:     return "Caret";
        case AnnotationType::FreeText:  return "FreeText";
        case AnnotationType::Polygon:   return "Polygon";
        case AnnotationType::PolyLine:  return "PolyLine";
        case AnnotationType::Stamp:     return "Stamp";
    }
    return "Square";
}

// 標準圖章的 /Name 值。Custom 回傳 nullptr——自訂圖章不寫 /Name，
// 因為那個鍵的值域是規格定死的，塞自訂字串進去等於產出別人讀不懂的檔案。
[[nodiscard]] constexpr const char* stampNameOf(StampKind kind) noexcept {
    switch (kind) {
        case StampKind::Approved:            return "Approved";
        case StampKind::Experimental:        return "Experimental";
        case StampKind::NotApproved:         return "NotApproved";
        case StampKind::AsIs:                return "AsIs";
        case StampKind::Expired:             return "Expired";
        case StampKind::Draft:               return "Draft";
        case StampKind::Final:               return "Final";
        case StampKind::Confidential:        return "Confidential";
        case StampKind::ForComment:          return "ForComment";
        case StampKind::TopSecret:           return "TopSecret";
        case StampKind::ForPublicRelease:    return "ForPublicRelease";
        case StampKind::NotForPublicRelease: return "NotForPublicRelease";
        case StampKind::Sold:                return "Sold";
        case StampKind::Departmental:        return "Departmental";
        case StampKind::Custom:              break;
    }
    return nullptr;
}

// 圖章上要畫的字。內建圖章的外觀由我們自己畫（PDFium 不產生 /Stamp 的 /AP），
// 而規格只定義了 /Name，沒有定義長什麼樣——各家檢視器畫得都不一樣，
// 所以「畫成什麼」是我們的選擇，只要語意對得上。
[[nodiscard]] constexpr const char* stampLabelOf(StampKind kind) noexcept {
    switch (kind) {
        case StampKind::Approved:            return "APPROVED";
        case StampKind::Experimental:        return "EXPERIMENTAL";
        case StampKind::NotApproved:         return "NOT APPROVED";
        case StampKind::AsIs:                return "AS IS";
        case StampKind::Expired:             return "EXPIRED";
        case StampKind::Draft:               return "DRAFT";
        case StampKind::Final:               return "FINAL";
        case StampKind::Confidential:        return "CONFIDENTIAL";
        case StampKind::ForComment:          return "FOR COMMENT";
        case StampKind::TopSecret:           return "TOP SECRET";
        case StampKind::ForPublicRelease:    return "FOR PUBLIC RELEASE";
        case StampKind::NotForPublicRelease: return "NOT FOR PUBLIC RELEASE";
        case StampKind::Sold:                return "SOLD";
        case StampKind::Departmental:        return "DEPARTMENTAL";
        case StampKind::Custom:              break;
    }
    return "";
}

// 圖章的預設顏色。核可類綠色、否決類紅色、其餘藍色——這是各家檢視器的共同慣例，
// 使用者不必讀字就能從顏色判斷方向。顏色不單獨承載意義：字本身也在。
[[nodiscard]] inline ColorRgb stampDefaultColor(StampKind kind) noexcept {
    switch (kind) {
        case StampKind::Approved:
        case StampKind::Final:
        case StampKind::ForPublicRelease:
        case StampKind::Sold:
            return ColorRgb{0.11, 0.50, 0.23};
        case StampKind::NotApproved:
        case StampKind::Expired:
        case StampKind::TopSecret:
        case StampKind::Confidential:
        case StampKind::NotForPublicRelease:
            return ColorRgb{0.75, 0.19, 0.17};
        default:
            break;
    }
    return ColorRgb{0.13, 0.35, 0.67};
}

[[nodiscard]] constexpr const char* freeTextIntentName(FreeTextIntent intent) noexcept {
    switch (intent) {
        case FreeTextIntent::TextBox:    return "FreeText";
        case FreeTextIntent::Typewriter: return "FreeTextTypewriter";
        case FreeTextIntent::Callout:    return "FreeTextCallout";
    }
    return "FreeText";
}

[[nodiscard]] constexpr bool isTextMarkup(AnnotationType type) noexcept {
    return type == AnnotationType::Highlight || type == AnnotationType::Underline ||
           type == AnnotationType::StrikeOut || type == AnnotationType::Squiggly;
}

// 一則註解的完整狀態。
//
// 欄位命名對應 PDF 字典鍵（見各欄註記），這樣寫入層可以逐欄對照 CLAUDE.md
// 列出的必填清單，漏欄在 code review 時看得出來。
struct Annotation {
    std::string id{};                          // /NM，同一文件內唯一
    RectF rect{};                              // /Rect；文字標記與 Ink 由幾何推導，可留空
    ColorRgb color{1.0, 0.85, 0.0};            // /C，描邊或標記主色
    std::optional<ColorRgb> interiorColor{};   // /IC，幾何註解的填色；無值代表不填
    double opacity{1.0};                       // /CA，0–1
    BorderStyle border{};                      // /BS
    AnnotationFlag flags{AnnotationFlag::Print};  // /F
    std::string author{};                      // /T
    std::string contents{};                    // /Contents
    std::string subject{};                     // /Subj
    PdfDate creationDate{};                    // /CreationDate
    PdfDate modifiedDate{};                    // /M
    std::optional<std::string> inReplyTo{};    // /IRT，回覆串（PRD-ANN-007）
    AnnotationGeometry geometry{TextMarkupGeometry{}};

    // /Measure（ISO 32000-1 §12.5.6.11）。只在 Line／Polygon／PolyLine 且已
    // 校正比例時才有值（PRD-ANN-014 / 024）。無值不代表「未量測」，
    // 只代表這則註解不帶量測比例——寫入層與讀取層都必須以此為準，
    // 不得在無值時假設任何預設比例。
    std::optional<MeasureInfo> measure{};

    [[nodiscard]] AnnotationType type() const noexcept { return typeOf(geometry); }
};

// 由頁面座標的矩形建一組 quad。文字擷取層給出的字元框是軸對齊矩形，走這條路。
[[nodiscard]] inline QuadPoint quadFromPageRect(const RectF& r) noexcept {
    const RectF n = r.normalized();
    return QuadPoint{
        PointF{n.left, n.top},
        PointF{n.right, n.top},
        PointF{n.left, n.bottom},
        PointF{n.right, n.bottom},
    };
}

// 由裝置（螢幕）座標的矩形建一組 quad。
//
// 這是旋轉頁面下 QuadPoints 正確與否的唯一關卡（PRD-ANN-001 驗收條件）。
// 裝置矩形在旋轉後對應到頁面上的哪四個角，交給 PageTransform 決定；
// 旋轉 90/270 度時「裝置的左上角」會落到頁面的其他角，因此不能只轉兩個對角點
// 再重組矩形——那樣得到的 quad 角序會錯。這裡四個角各自轉換後再依頁面
// 座標系重新指派上下左右。
[[nodiscard]] inline QuadPoint quadFromDeviceRect(const PageTransform& transform,
                                                  const RectF& deviceRect) noexcept {
    const RectF d = deviceRect.normalized();
    const PointF corners[4] = {
        transform.toPage({d.left, d.bottom}),
        transform.toPage({d.right, d.bottom}),
        transform.toPage({d.left, d.top}),
        transform.toPage({d.right, d.top}),
    };

    RectF bounds{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const PointF& p : corners) {
        bounds.left = std::min(bounds.left, p.x);
        bounds.right = std::max(bounds.right, p.x);
        bounds.bottom = std::min(bounds.bottom, p.y);
        bounds.top = std::max(bounds.top, p.y);
    }
    return quadFromPageRect(bounds);
}

// 把一條手繪路徑轉成一串 QuadPoints（PRD-ANN-018 Free Highlight）。
//
// Free Highlight 在 Acrobat 是 /Subtype /Highlight，不是 /Ink：螢光筆的規格
// 只認 QuadPoints，用 InkList 表達的話多數檢視器不會把它畫成半透明色塊。
// 因此手繪路徑要先「拉直」成一串矩形色帶，每段一個 quad，再交給既有的
// TextMarkupGeometry{Highlight, quads} 走完全相同的外觀產生與寫入路徑。
//
// 每個 quad 只覆蓋自己那一段，轉角處會有一個三角形縫隙（未做圓角處理）；
// 螢光筆本身半透明、縫隙寬度通常小於一個像素，實務上看不出來，
// 是刻意的簡化而非疏漏。
[[nodiscard]] inline std::vector<QuadPoint> ribbonQuadsFromStroke(
    const std::vector<PointF>& points, double halfWidth) noexcept {
    std::vector<QuadPoint> quads;
    if (points.size() < 2 || !(halfWidth > 0.0)) return quads;
    quads.reserve(points.size() - 1);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const PointF& p0 = points[i];
        const PointF& p1 = points[i + 1];
        const double dx = p1.x - p0.x;
        const double dy = p1.y - p0.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 0.0)) continue;  // 重複點：這一段沒有方向，跳過而不是產生退化 quad
        const double nx = -dy / len * halfWidth;
        const double ny = dx / len * halfWidth;
        quads.push_back(QuadPoint{
            PointF{p0.x + nx, p0.y + ny},  // upperLeft
            PointF{p1.x + nx, p1.y + ny},  // upperRight
            PointF{p0.x - nx, p0.y - ny},  // lowerLeft
            PointF{p1.x - nx, p1.y - ny},  // lowerRight
        });
    }
    return quads;
}

[[nodiscard]] inline RectF boundsOfQuads(const std::vector<QuadPoint>& quads) noexcept {
    RectF bounds{};
    bool first = true;
    for (const QuadPoint& q : quads) {
        const PointF pts[4] = {q.upperLeft, q.upperRight, q.lowerLeft, q.lowerRight};
        for (const PointF& p : pts) {
            if (first) {
                bounds = RectF{p.x, p.y, p.x, p.y};
                first = false;
            } else {
                bounds.left = std::min(bounds.left, p.x);
                bounds.right = std::max(bounds.right, p.x);
                bounds.bottom = std::min(bounds.bottom, p.y);
                bounds.top = std::max(bounds.top, p.y);
            }
        }
    }
    return bounds;
}

[[nodiscard]] inline RectF boundsOfStrokes(const std::vector<std::vector<PointF>>& strokes) noexcept {
    RectF bounds{};
    bool first = true;
    for (const auto& stroke : strokes) {
        for (const PointF& p : stroke) {
            if (first) {
                bounds = RectF{p.x, p.y, p.x, p.y};
                first = false;
            } else {
                bounds.left = std::min(bounds.left, p.x);
                bounds.right = std::max(bounds.right, p.x);
                bounds.bottom = std::min(bounds.bottom, p.y);
                bounds.top = std::max(bounds.top, p.y);
            }
        }
    }
    return bounds;
}

// 註解列表面板要顯示的摘要（PRD-ANN-008）。
//
// 刻意不是完整的 Annotation：列表只需要顯示與跳轉用的欄位，
// 而 1,000 筆註解要在 500 毫秒內載完，把每則註解的完整幾何都讀出來做不到。
struct AnnotationSummary {
    std::int32_t pageIndex{0};
    std::int32_t indexOnPage{0};
    std::string subtype;   // /Subtype 的原始名稱，例如 Highlight
    std::string author;    // /T
    std::string contents;  // /Contents
    std::string modified;  // /M，原始 PDF 日期字串
    RectF rect{};
};

}  // namespace alioth::domain
