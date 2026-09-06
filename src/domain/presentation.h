#pragma once

// 全螢幕／簡報模式與頁面轉場的純邏輯核心（PRD-VIEW-009）。
//
// 狀態機與轉場緩動函式故意與 Qt 全螢幕視窗 API 分開：後者只在真正的視窗上才能
// 測，前者不需要——「F11 該不該把視窗切到全螢幕」與「Esc 只能從全螢幕/簡報
// 模式回到一般模式」是可以離線驗證的規則，混進 QMainWindow 的子類別裡就只能
// 靠人工點一遍。

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace alioth::domain {

enum class ViewMode : std::uint8_t {
    Normal,
    Fullscreen,   // 保留 Ribbon 與面板，只是視窗佔滿螢幕
    Presentation, // 隱藏一切介面元素，只顯示頁面內容
};

enum class PageTransition : std::uint8_t {
    None,
    Fade,
    SlideHorizontal,
};

// F11 在 Normal 與 Fullscreen 間切換；簡報模式另有專屬進入點（例如 Ribbon 按鈕
// 或 Shift+F11），退出一律回到「進入前的模式」而不是永遠回到 Normal——
// 從 Fullscreen 進簡報模式再退出，應該停在 Fullscreen 而不是跳回一般視窗。
class PresentationController {
public:
    [[nodiscard]] ViewMode mode() const noexcept { return mode_; }

    void toggleFullscreen() {
        if (mode_ == ViewMode::Normal) {
            previous_ = mode_;
            mode_ = ViewMode::Fullscreen;
        } else if (mode_ == ViewMode::Fullscreen) {
            mode_ = previous_;
        }
        // Presentation 模式下 F11 不生效：簡報中途切全螢幕沒有意義，
        // 使用者要的退出鍵是 Esc。
    }

    void enterPresentation() {
        if (mode_ == ViewMode::Presentation) return;
        previous_ = mode_;
        mode_ = ViewMode::Presentation;
    }

    // Esc：僅在 Fullscreen 或 Presentation 時生效，回到進入前的模式。
    // Normal 模式下呼叫是no-op——呼叫端仍可以把 Esc 挪去做別的事（例如取消選取）。
    [[nodiscard]] bool handleEscape() {
        if (mode_ == ViewMode::Normal) return false;
        mode_ = (mode_ == ViewMode::Presentation) ? previous_ : ViewMode::Normal;
        return true;
    }

private:
    ViewMode mode_{ViewMode::Normal};
    ViewMode previous_{ViewMode::Normal};
};

// 轉場進度緩動：輸入已耗費時間與總時長，輸出 [0,1] 的 easeInOutCubic 進度。
// 呼叫端用它去插值透明度（Fade）或位移比例（SlideHorizontal）。
// elapsedMs 為負或 durationMs <= 0 時回傳 1.0（視為已完成，不阻塞翻頁）。
[[nodiscard]] inline double transitionProgress(double elapsedMs, double durationMs) noexcept {
    if (durationMs <= 0.0) return 1.0;
    const double t = std::clamp(elapsedMs / durationMs, 0.0, 1.0);
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

// Fade 轉場：新頁淡入的不透明度。
[[nodiscard]] inline double fadeOpacity(double progress) noexcept {
    return std::clamp(progress, 0.0, 1.0);
}

// SlideHorizontal 轉場：新頁與舊頁的水平位移比例（乘上可視區寬度即得像素位移）。
// direction 為 true 表示往下一頁（新頁從右滑入），false 表示往上一頁。
struct SlideOffsets {
    double outgoing{0.0};  // 舊頁位移比例
    double incoming{0.0};  // 新頁位移比例
};

[[nodiscard]] inline SlideOffsets slideOffsets(double progress, bool forward) noexcept {
    const double p = std::clamp(progress, 0.0, 1.0);
    const double sign = forward ? 1.0 : -1.0;
    return SlideOffsets{-sign * p, sign * (1.0 - p)};
}

}  // namespace alioth::domain
