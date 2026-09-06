#pragma once

// WP24 註解工具家族的建構器（應用層薄薄一層）。
//
// PRD-ANN-005 Text Box、PRD-ANN-017 Highlight Area、PRD-ANN-018 Free Highlight、
// PRD-ANN-019 Caret、PRD-ANN-021 Typewriter、PRD-ANN-022 Callout、
// PRD-ANN-030 Fit Box by Text Content。
//
// 這裡只把「使用者在畫布上做的動作」（拖一個矩形、畫一條手繪路徑、打一段字）
// 轉成合法的 domain::Annotation，不處理事件、不碰檔案。落地寫入一律交給既有的
// app::AnnotationService::addAnnotation——那條路徑已經做完開檔、ADR-002 物件層
// 寫入、增量驗證、原子寫檔、復原邊界，這裡重做一次只會製造第二份真相（IL-3）。
//
// 刻意不繼承 QObject、不含事件處理，因此可以用最陽春的 QtTest 測試。

#include <optional>
#include <string>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"

namespace alioth::app {

// PRD-ANN-017 Highlight Area：使用者直接拖一個矩形，不依賴文字層，
// 因此掃描件（沒有文字層的 PDF）也能用。落地後與一般螢光筆共用同一個
// /Subtype /Highlight，Acrobat 端沒有分別。
struct HighlightAreaRequest {
    domain::RectF pageRect;  // 使用者拖曳出的矩形，頁面座標
    domain::ColorRgb color{1.0, 0.85, 0.0};
    double opacity{0.4};
};
[[nodiscard]] domain::Annotation buildHighlightArea(const HighlightAreaRequest& request);

// PRD-ANN-018 Free Highlight：手繪路徑轉成一串 QuadPoints（見
// domain::ribbonQuadsFromStroke），同樣落地成 /Subtype /Highlight。
struct FreeHighlightRequest {
    std::vector<domain::PointF> strokePoints;  // 頁面座標，至少兩點
    double halfWidth{6.0};                     // 筆刷半寬，單位點
    domain::ColorRgb color{1.0, 0.85, 0.0};
    double opacity{0.4};
};
struct FreeHighlightBuildResult {
    bool ok{false};
    std::string diagnostic;
    domain::Annotation annotation;
};
[[nodiscard]] FreeHighlightBuildResult buildFreeHighlight(const FreeHighlightRequest& request);

// PRD-ANN-019 Caret：校正符號，None（插入 ^）或 Paragraph（新段落 ¶）。
struct CaretRequest {
    domain::RectF rect;
    domain::CaretSymbol symbol{domain::CaretSymbol::None};
    domain::ColorRgb color{0.0, 0.0, 0.0};
};
[[nodiscard]] domain::Annotation buildCaret(const CaretRequest& request);

// Text Box（PRD-ANN-005）／Typewriter（PRD-ANN-021）共用的請求：兩者的差異
// 只在 intent（由呼叫端指定，buildTextBox/buildTypewriter 各自帶入正確值），
// 外觀產生器（engine/annotations/appearance_stream.cpp）依 intent 決定
// Typewriter 強制不畫框。
struct FreeTextRequest {
    domain::RectF rect;   // rect.width() <= 0 代表尚未拖出寬度，autoFit 決定寬度
    std::string text;
    double fontSize{12.0};
    domain::ColorRgb textColor{0.0, 0.0, 0.0};
    domain::ColorRgb borderColor{0.0, 0.0, 0.0};
    std::optional<domain::ColorRgb> fillColor{};
    double borderWidth{1.0};
    domain::TextAlign align{domain::TextAlign::Left};
    // PRD-ANN-030 Fit Box by Text Content：依內容重算高度（寬度未定時也重算寬度）。
    bool autoFit{true};
    // 固定框：使用者已經把框拖成想要的大小，內容塞不下時**縮字**而不是撐框。
    // 撐框會蓋掉底下的內容，而那是使用者剛剛才刻意避開的。
    bool fixedBox{false};
};

struct FreeTextBuildResult {
    bool ok{false};
    std::string diagnostic;
    domain::Annotation annotation;
    // 固定框模式下實際採用的字級，以及「縮到最小仍塞不下」。
    // 溢出必須讓使用者看見——靜默裁掉的文字沒有任何跡象可循。
    double fontSize{0.0};
    bool overflows{false};
};

[[nodiscard]] FreeTextBuildResult buildTextBox(const FreeTextRequest& request);
[[nodiscard]] FreeTextBuildResult buildTypewriter(const FreeTextRequest& request);

// PRD-ANN-022 Callout：文字框 + 指向頁面上某處的引線。
//
// 引線起點（貼在文字框邊上的那一端）由 rect 與 target/knee 自動算出——
// 使用者操作的是「文字框」與「引線指向哪裡」，不會手動去點框緣上的一個像素，
// 這與 Acrobat 的行為一致：拖曳目標時起點自動吸附到最近的框緣。
struct CalloutRequest {
    domain::RectF rect;
    domain::PointF target;               // 引線終點：箭頭指向的內容
    std::optional<domain::PointF> knee;  // 折點，可選
    std::string text;
    double fontSize{12.0};
    domain::ColorRgb textColor{0.0, 0.0, 0.0};
    domain::ColorRgb borderColor{0.0, 0.0, 0.0};
    std::optional<domain::ColorRgb> fillColor{};
    double borderWidth{1.0};
    domain::TextAlign align{domain::TextAlign::Left};
    domain::LineEnding ending{domain::LineEnding::OpenArrow};
    bool autoFit{true};
    bool fixedBox{false};
};
[[nodiscard]] FreeTextBuildResult buildCallout(const CalloutRequest& request);

// 矩形邊界上離 target 最近的一點（PRD-ANN-022「引線端點可拖曳」的自動吸附側）。
// 若 target 本身落在矩形內部（少見但可能發生，例如目標與框重疊），
// 貼到最近的一條邊而不是回傳 target 本身——引線起點必須在框緣上，
// 不能是框內部的點，否則 /CL 畫出來會有一段線埋在框裡看不見。
[[nodiscard]] domain::PointF nearestRectBoundaryPoint(const domain::RectF& rect,
                                                      const domain::PointF& target) noexcept;

}  // namespace alioth::app
