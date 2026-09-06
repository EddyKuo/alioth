#pragma once

// 淺色／深色主題（PRD-UI-006）。
//
// 色票以資料描述（ColorToken 陣列），不是散落在各個 widget 建構式裡的
// setStyleSheet 字串——散落的寫法會讓「深色模式下這個面板忘了改」變成常態，
// 而且無從測試對比度。ThemePalette 是純資料 + 純函數，不依賴 QApplication，
// 可以在無 GUI 環境下驗證 WCAG AA 對比度。
//
// 跟隨系統交給 ThemeController：Qt 6.5+ 的 QStyleHints::colorScheme() /
// colorSchemeChanged 訊號提供了系統主題變化通知，ThemeController 訂閱它並在
// 變化時重算色票、發出 themeChanged，讓呈現層重繪。

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QString>

#include <array>
#include <vector>

namespace alioth::app {

enum class ThemeMode {
    Light,
    Dark,
    System  // 跟隨作業系統，實際套用的是 Light 或 Dark 其中之一
};

// 色票角色。刻意只列出目前呈現層真的會用到的角色，角色名稱與 QPalette 的
// ColorRole 對齊之處直接借用其語意，其餘（Highlight 等）比照。
enum class ColorRole {
    WindowBackground,
    WindowText,
    Base,          // 內容區背景（例如頁面檢視外的空白）
    AlternateBase,
    Button,
    ButtonText,
    Highlight,
    HighlightedText,
    Link,
    ToolTipBase,
    ToolTipText,
};

struct ColorToken {
    ColorRole role;
    QColor color;
};

class ThemePalette {
public:
    static ThemePalette light();
    static ThemePalette dark();
    static ThemePalette forMode(ThemeMode resolvedMode);  // resolvedMode 不可為 System

    [[nodiscard]] QColor color(ColorRole role) const;
    [[nodiscard]] QPalette toQPalette() const;

    // WCAG AA 對比度計算（相對亮度公式，見 WCAG 2.1 §1.4.3）。回傳值 >= 4.5
    // 才符合一般文字的 AA 門檻。
    static double contrastRatio(const QColor& foreground, const QColor& background);

    // 驗證這份色票裡「文字 / 對應背景」的每一對组合是否都達到 4.5:1。
    // 回傳所有不合格的組合描述；空陣列代表全數合格。
    [[nodiscard]] std::vector<QString> validateContrast() const;

private:
    std::array<ColorToken, 11> tokens_;
};

QString toString(ColorRole role);

}  // namespace alioth::app
