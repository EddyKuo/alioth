#include "app/annotation_family_builders.h"

#include <algorithm>
#include <cmath>

#include "engine/annotations/text_layout.h"

namespace alioth::app {

namespace {

using engine::annotations::FitBoxOptions;
using engine::annotations::FitBoxResult;
using engine::annotations::fitBoxByTextContent;
using engine::annotations::kFreeTextPaddingPt;

domain::Annotation baseFreeText(const domain::RectF& rect, domain::FreeTextIntent intent,
                                const std::string& text, double fontSize,
                                const domain::ColorRgb& textColor,
                                const domain::ColorRgb& borderColor,
                                const std::optional<domain::ColorRgb>& fillColor,
                                double borderWidth, domain::TextAlign align) {
    domain::Annotation annotation;
    annotation.rect = rect;
    annotation.color = borderColor;
    annotation.interiorColor = fillColor;
    annotation.border.width = intent == domain::FreeTextIntent::Typewriter ? 0.0 : borderWidth;

    domain::FreeTextGeometry geometry;
    geometry.text = text;
    geometry.fontSize = fontSize;
    geometry.textColor = textColor;
    geometry.align = align;
    geometry.intent = intent;
    annotation.geometry = std::move(geometry);
    return annotation;
}

// 套用 Fit Box by Text Content（PRD-ANN-030）：呼叫端要求 autoFit 時，
// 依文字內容重算 rect；不需要或量測失敗時原樣沿用呼叫端給的 rect，
// 讓錯誤原因（例如非 ASCII 文字）留給下游的外觀產生器統一回報，
// 而不是在這裡就吞掉——那樣會讓「為什麼框沒有變」變成一個要翻程式碼才知道的謎。
FreeTextBuildResult applyAutoFitIfRequested(domain::Annotation annotation, bool autoFit,
                                            const std::string& text, double fontSize,
                                            bool fixedBox) {
    FreeTextBuildResult result;
    result.fontSize = fontSize;
    if (autoFit) {
        FitBoxOptions options{};
        options.fontSize = fontSize;
        options.paddingPt = kFreeTextPaddingPt;
        // 固定框：高度不動，改成縮字（PRD-ANN-030）。框已經是使用者拖出來的
        // 大小，把它撐高會蓋掉他剛剛刻意避開的內容。
        if (fixedBox) options.maxHeight = annotation.rect.normalized().height();
        const FitBoxResult fit = fitBoxByTextContent(annotation.rect, text, options);
        if (fit.ok) {
            annotation.rect = fit.rect;
            result.fontSize = fit.fontSize;
            result.overflows = fit.overflows;
            // 縮字之後外觀產生器要用同一個字級畫，否則框是縮過的、字沒縮。
            if (auto* geometry = std::get_if<domain::FreeTextGeometry>(&annotation.geometry)) {
                geometry->fontSize = fit.fontSize;
            }
        }
        // fit.ok 為 false 時（例如非 ASCII）刻意不動 rect，讓 writeAnnotation
        // 呼叫外觀產生器時用同一組輸入再失敗一次，錯誤訊息才會一致。
    }
    result.ok = true;
    result.annotation = std::move(annotation);
    return result;
}

}  // namespace

domain::Annotation buildHighlightArea(const HighlightAreaRequest& request) {
    domain::TextMarkupGeometry geometry;
    geometry.kind = domain::TextMarkupKind::Highlight;
    geometry.quads.push_back(domain::quadFromPageRect(request.pageRect));

    domain::Annotation annotation;
    annotation.color = request.color;
    annotation.opacity = request.opacity;
    annotation.geometry = std::move(geometry);
    return annotation;
}

FreeHighlightBuildResult buildFreeHighlight(const FreeHighlightRequest& request) {
    FreeHighlightBuildResult result;
    if (!(request.halfWidth > 0.0)) {
        result.diagnostic = "筆刷寬度必須大於零";
        return result;
    }
    const std::vector<domain::QuadPoint> quads =
        domain::ribbonQuadsFromStroke(request.strokePoints, request.halfWidth);
    if (quads.empty()) {
        result.diagnostic = "手繪路徑至少要兩個不重合的點";
        return result;
    }

    domain::TextMarkupGeometry geometry;
    geometry.kind = domain::TextMarkupKind::Highlight;
    geometry.quads = quads;

    domain::Annotation annotation;
    annotation.color = request.color;
    annotation.opacity = request.opacity;
    annotation.geometry = std::move(geometry);

    result.ok = true;
    result.annotation = std::move(annotation);
    return result;
}

domain::Annotation buildCaret(const CaretRequest& request) {
    domain::Annotation annotation;
    annotation.rect = request.rect;
    annotation.color = request.color;
    annotation.geometry = domain::CaretGeometry{request.symbol};
    return annotation;
}

FreeTextBuildResult buildTextBox(const FreeTextRequest& request) {
    domain::Annotation annotation =
        baseFreeText(request.rect, domain::FreeTextIntent::TextBox, request.text,
                    request.fontSize, request.textColor, request.borderColor, request.fillColor,
                    request.borderWidth, request.align);
    return applyAutoFitIfRequested(std::move(annotation), request.autoFit, request.text,
                                   request.fontSize, request.fixedBox);
}

FreeTextBuildResult buildTypewriter(const FreeTextRequest& request) {
    domain::Annotation annotation =
        baseFreeText(request.rect, domain::FreeTextIntent::Typewriter, request.text,
                    request.fontSize, request.textColor, request.borderColor, request.fillColor,
                    request.borderWidth, request.align);
    return applyAutoFitIfRequested(std::move(annotation), request.autoFit, request.text,
                                   request.fontSize, request.fixedBox);
}

domain::PointF nearestRectBoundaryPoint(const domain::RectF& rect,
                                        const domain::PointF& target) noexcept {
    const domain::RectF r = rect.normalized();
    const bool insideX = target.x >= r.left && target.x <= r.right;
    const bool insideY = target.y >= r.bottom && target.y <= r.top;

    if (insideX && insideY) {
        // target 落在框內：貼到最近的一條邊，而不是回傳框內部的點——
        // 引線起點必須在框緣上，否則 /CL 會有一段線畫在框裡看不見。
        const double distLeft = target.x - r.left;
        const double distRight = r.right - target.x;
        const double distBottom = target.y - r.bottom;
        const double distTop = r.top - target.y;
        const double nearest = std::min({distLeft, distRight, distBottom, distTop});
        if (nearest == distLeft) return domain::PointF{r.left, target.y};
        if (nearest == distRight) return domain::PointF{r.right, target.y};
        if (nearest == distBottom) return domain::PointF{target.x, r.bottom};
        return domain::PointF{target.x, r.top};
    }

    // target 在框外：矩形是凸集，把座標各自夾進範圍內就是框上（含邊界）最近的點。
    const double cx = std::clamp(target.x, r.left, r.right);
    const double cy = std::clamp(target.y, r.bottom, r.top);
    return domain::PointF{cx, cy};
}

FreeTextBuildResult buildCallout(const CalloutRequest& request) {
    domain::Annotation annotation =
        baseFreeText(request.rect, domain::FreeTextIntent::Callout, request.text,
                    request.fontSize, request.textColor, request.borderColor, request.fillColor,
                    request.borderWidth, request.align);

    // 先貼合內容再算引線起點：Fit Box 只會動 rect 的 bottom（高度），
    // 若起點恰好落在下緣，先算後貼合會讓起點停在舊的下緣、不再貼著新框，
    // /CL 因此會有一段線畫在框外。
    FreeTextBuildResult fitted =
        applyAutoFitIfRequested(std::move(annotation), request.autoFit, request.text,
                                request.fontSize, request.fixedBox);
    if (!fitted.ok) return fitted;

    domain::CalloutLine line;
    line.start =
        nearestRectBoundaryPoint(fitted.annotation.rect, request.knee.value_or(request.target));
    line.knee = request.knee;
    line.end = request.target;
    line.ending = request.ending;

    auto& geometry = std::get<domain::FreeTextGeometry>(fitted.annotation.geometry);
    geometry.callout = line;
    return fitted;
}

}  // namespace alioth::app
