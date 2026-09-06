#pragma once

// 主題控制器（PRD-UI-006）：把 ThemeMode（含「跟隨系統」）解析成實際套用的
// ThemePalette，並在系統主題變化時真的更新——不只是啟動時讀一次。
//
// 「跟隨系統」走 QStyleHints::colorScheme() / colorSchemeChanged 訊號（Qt 6.5+，
// 本專案是 6.8）。這個 controller 本身不碰任何 widget，套用（setPalette）
// 交給呼叫端在收到 themeChanged 時執行，維持「widget 只是薄殼」。

#include <QObject>

#include "app/uisystem/theme_palette.h"

class QGuiApplication;

namespace alioth::app {

class ThemeController : public QObject {
    Q_OBJECT

public:
    explicit ThemeController(QObject* parent = nullptr);

    void setMode(ThemeMode mode);
    [[nodiscard]] ThemeMode mode() const { return mode_; }

    // 目前實際套用的模式：mode() == System 時由系統色彩方案解析而來。
    [[nodiscard]] ThemeMode resolvedMode() const { return resolvedMode_; }
    [[nodiscard]] ThemePalette currentPalette() const { return ThemePalette::forMode(resolvedMode_); }

    // 供測試與非 GUI 情境注入系統目前的色彩方案，不依賴真的作業系統事件。
    void setSystemDarkModeForTesting(bool dark);

signals:
    void themeChanged(ThemeMode resolvedMode);

private:
    void recomputeResolvedMode();

    ThemeMode mode_{ThemeMode::System};
    ThemeMode resolvedMode_{ThemeMode::Light};
    bool systemIsDark_{false};
};

}  // namespace alioth::app
