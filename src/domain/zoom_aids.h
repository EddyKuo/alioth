#pragma once

// 區域框選縮放、Loupe 放大鏡、Pan & Zoom 面板的純邏輯核心
// （PRD-ZOOM-004、PRD-ZOOM-005）。
//
// 三者共通的硬性限制（docs/SDD.md 架構限制 2）：嚴禁整頁光柵化。這裡的函式
// 一律只算「該渲染哪一小塊」，實際渲染仍然透過既有的圖磚管線
// （app::DocumentController::scheduleTiles / tileIfReady）取得——Loupe 用
// 高倍率的一塊小圖磚，Pan & Zoom 面板用縮圖（縮圖是架構文件明列的例外）。
//
// 一律在「文件裝置空間」（原點左上、Y 向下、單位像素，見 geometry.h）operate，
// 呼叫端负责把使用者輸入（widget 座標）先扣掉捲動位移換算成文件裝置座標。

#include <algorithm>
#include <cmath>

#include "domain/geometry.h"

namespace alioth::domain {

// ---- 區域框選縮放（PRD-ZOOM-004）----

struct RectZoomRequest {
    RectI selection;    // 使用者框選的區域，文件裝置空間，目前倍率下
    double currentScale{1.0};
    SizeF viewportPx{}; // 檢視可視區大小
    double minScale{0.1};
    double maxScale{64.0};
};

struct RectZoomResult {
    double newScale{1.0};
    // 框選區域中心點，換算到「新倍率下的文件裝置空間」座標——呼叫端拿它
    // 當捲動目標（使新的可視區中心對齊這一點）。
    PointF centerAtNewScale{};
};

// 讓框選區域填滿可視區（PRD-ZOOM-004 驗收標準）：取寬高兩個縮放比例中較小者，
// 避免任一方向超出可視區。框選區域退化（寬或高為 0）時視為無效請求，
// 回傳目前倍率、不移動——呼叫端應該忽略單點點擊觸發的框選。
[[nodiscard]] inline RectZoomResult computeRectZoom(const RectZoomRequest& request) {
    RectZoomResult result;
    result.newScale = request.currentScale;
    if (request.selection.isEmpty() || request.viewportPx.isEmpty() ||
        request.currentScale <= 0.0) {
        return result;
    }

    const double scaleX = request.viewportPx.width / static_cast<double>(request.selection.width);
    const double scaleY =
        request.viewportPx.height / static_cast<double>(request.selection.height);
    double newScale = request.currentScale * std::min(scaleX, scaleY);
    newScale = std::clamp(newScale, request.minScale, request.maxScale);

    const double factor = newScale / request.currentScale;
    const PointF centerAtCurrentScale{
        request.selection.x + request.selection.width / 2.0,
        request.selection.y + request.selection.height / 2.0,
    };
    result.newScale = newScale;
    result.centerAtNewScale = {centerAtCurrentScale.x * factor, centerAtCurrentScale.y * factor};
    return result;
}

// ---- Loupe 放大鏡（PRD-ZOOM-004）----

struct LoupeRequest {
    PointF cursorAtBaseScale{};  // 游標在目前（基準）倍率下的文件裝置座標
    double baseScale{1.0};
    double magnification{2.0};   // 相對於 baseScale 的額外倍率，例如 2.0 = 兩倍
    SizeF loupeWidgetPx{};       // 放大鏡視窗大小（像素）
};

struct LoupeSample {
    double effectiveScale{1.0};      // baseScale * magnification，用來排圖磚請求
    // 要向圖磚管線請求的可視區：游標位置在「有效倍率」下的座標，置中於這塊
    // loupeWidgetPx 大小的矩形。呼叫端把這個矩形當成一個迷你可視區丟給
    // DocumentController::scheduleTiles，不是另開一條整頁光柵化路徑。
    RectI sampleRect;
};

[[nodiscard]] inline LoupeSample computeLoupeSample(const LoupeRequest& request) {
    LoupeSample sample;
    if (request.baseScale <= 0.0 || request.magnification <= 0.0) return sample;
    sample.effectiveScale = request.baseScale * request.magnification;
    const double factor = request.magnification;
    const PointF centerAtEffectiveScale{request.cursorAtBaseScale.x * factor,
                                        request.cursorAtBaseScale.y * factor};
    const int w = static_cast<int>(std::lround(request.loupeWidgetPx.width));
    const int h = static_cast<int>(std::lround(request.loupeWidgetPx.height));
    sample.sampleRect = RectI{
        static_cast<std::int32_t>(std::lround(centerAtEffectiveScale.x - w / 2.0)),
        static_cast<std::int32_t>(std::lround(centerAtEffectiveScale.y - h / 2.0)),
        w,
        h,
    };
    return sample;
}

// ---- Pan & Zoom 面板（PRD-ZOOM-005）----

// 面板顯示整份文件（或目前頁）縮小後的縮圖，並用一個可拖曳的矩形表示主視圖
// 目前看得到的範圍。面板尺寸與主視圖的內容尺寸兩者比例不同，因此縮圖倍率取
// 「整個內容都塞得進面板」的那個比例（等比縮放，不裁切、不變形）。
[[nodiscard]] inline double panZoomThumbnailScale(SizeF contentSizePx, SizeF panelSizePx) {
    if (contentSizePx.isEmpty() || panelSizePx.isEmpty()) return 1.0;
    return std::min(panelSizePx.width / contentSizePx.width,
                    panelSizePx.height / contentSizePx.height);
}

// 面板縮圖座標系的矩形。刻意不用 RectF：RectF 的 left/bottom/right/top 命名
// 是頁面空間（Y 向上）的慣例，這裡是文件裝置空間（Y 向下），沿用會混淆方向。
// 也不用 RectI：面板縮圖倍率通常是小數，取整會讓往返測試累積誤差。
struct ThumbnailRectF {
    double x{0.0};
    double y{0.0};
    double width{0.0};
    double height{0.0};
};

// 主視圖可視區（文件裝置空間，主視圖倍率）→ 面板縮圖座標。
[[nodiscard]] inline ThumbnailRectF mainViewportToThumbnailRect(const RectI& mainViewport,
                                                                 double mainScale,
                                                                 double thumbnailScale) {
    if (mainScale <= 0.0) return {};
    const double factor = thumbnailScale / mainScale;
    return ThumbnailRectF{mainViewport.x * factor, mainViewport.y * factor,
                          mainViewport.width * factor, mainViewport.height * factor};
}

// 面板縮圖座標（使用者在面板上拖出/拖曳的矩形）→ 主視圖應該捲到的左上角
// （文件裝置空間，主視圖倍率）。這是上面那個函式的反函式，兩者必須互為逆運算，
// 否則面板拖一下、主視圖跳到別的地方——見 test_zoom_aids.cpp 的往返測試。
[[nodiscard]] inline PointF thumbnailRectToMainOrigin(const ThumbnailRectF& thumbnailRect,
                                                       double mainScale, double thumbnailScale) {
    if (thumbnailScale <= 0.0) return {};
    const double factor = mainScale / thumbnailScale;
    return {thumbnailRect.x * factor, thumbnailRect.y * factor};
}

}  // namespace alioth::domain
