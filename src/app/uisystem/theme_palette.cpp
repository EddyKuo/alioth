#include "app/uisystem/theme_palette.h"

#include <cmath>

namespace alioth::app {

namespace {

// sRGB → 相對亮度（WCAG 2.1 §1.4.3）。係數與轉折點是規格定值，不是估計值。
double srgbChannelToLinear(double channel) {
    return channel <= 0.03928 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& color) {
    const double r = srgbChannelToLinear(color.redF());
    const double g = srgbChannelToLinear(color.greenF());
    const double b = srgbChannelToLinear(color.blueF());
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

std::size_t indexOf(ColorRole role) { return static_cast<std::size_t>(role); }

}  // namespace

QString toString(ColorRole role) {
    switch (role) {
        case ColorRole::WindowBackground: return QStringLiteral("window_background");
        case ColorRole::WindowText: return QStringLiteral("window_text");
        case ColorRole::Base: return QStringLiteral("base");
        case ColorRole::AlternateBase: return QStringLiteral("alternate_base");
        case ColorRole::Button: return QStringLiteral("button");
        case ColorRole::ButtonText: return QStringLiteral("button_text");
        case ColorRole::Highlight: return QStringLiteral("highlight");
        case ColorRole::HighlightedText: return QStringLiteral("highlighted_text");
        case ColorRole::Link: return QStringLiteral("link");
        case ColorRole::ToolTipBase: return QStringLiteral("tooltip_base");
        case ColorRole::ToolTipText: return QStringLiteral("tooltip_text");
    }
    return QStringLiteral("window_background");
}

ThemePalette ThemePalette::light() {
    ThemePalette palette;
    palette.tokens_ = {{
        {ColorRole::WindowBackground, QColor(243, 243, 243)},
        {ColorRole::WindowText, QColor(26, 26, 26)},
        {ColorRole::Base, QColor(255, 255, 255)},
        {ColorRole::AlternateBase, QColor(234, 234, 234)},
        {ColorRole::Button, QColor(225, 225, 225)},
        {ColorRole::ButtonText, QColor(26, 26, 26)},
        {ColorRole::Highlight, QColor(0, 96, 192)},
        {ColorRole::HighlightedText, QColor(255, 255, 255)},
        {ColorRole::Link, QColor(0, 96, 192)},
        {ColorRole::ToolTipBase, QColor(255, 255, 220)},
        {ColorRole::ToolTipText, QColor(26, 26, 26)},
    }};
    return palette;
}

ThemePalette ThemePalette::dark() {
    ThemePalette palette;
    palette.tokens_ = {{
        {ColorRole::WindowBackground, QColor(32, 32, 32)},
        {ColorRole::WindowText, QColor(232, 232, 232)},
        {ColorRole::Base, QColor(24, 24, 24)},
        {ColorRole::AlternateBase, QColor(38, 38, 38)},
        {ColorRole::Button, QColor(51, 51, 51)},
        {ColorRole::ButtonText, QColor(232, 232, 232)},
        {ColorRole::Highlight, QColor(51, 153, 255)},
        {ColorRole::HighlightedText, QColor(10, 10, 10)},
        {ColorRole::Link, QColor(109, 179, 255)},
        {ColorRole::ToolTipBase, QColor(58, 58, 58)},
        {ColorRole::ToolTipText, QColor(240, 240, 240)},
    }};
    return palette;
}

ThemePalette ThemePalette::forMode(ThemeMode resolvedMode) {
    Q_ASSERT(resolvedMode != ThemeMode::System);
    return resolvedMode == ThemeMode::Dark ? dark() : light();
}

QColor ThemePalette::color(ColorRole role) const { return tokens_[indexOf(role)].color; }

QPalette ThemePalette::toQPalette() const {
    QPalette palette;
    palette.setColor(QPalette::Window, color(ColorRole::WindowBackground));
    palette.setColor(QPalette::WindowText, color(ColorRole::WindowText));
    palette.setColor(QPalette::Base, color(ColorRole::Base));
    palette.setColor(QPalette::AlternateBase, color(ColorRole::AlternateBase));
    palette.setColor(QPalette::Button, color(ColorRole::Button));
    palette.setColor(QPalette::ButtonText, color(ColorRole::ButtonText));
    palette.setColor(QPalette::Highlight, color(ColorRole::Highlight));
    palette.setColor(QPalette::HighlightedText, color(ColorRole::HighlightedText));
    palette.setColor(QPalette::Link, color(ColorRole::Link));
    palette.setColor(QPalette::ToolTipBase, color(ColorRole::ToolTipBase));
    palette.setColor(QPalette::ToolTipText, color(ColorRole::ToolTipText));
    palette.setColor(QPalette::Text, color(ColorRole::WindowText));
    return palette;
}

double ThemePalette::contrastRatio(const QColor& foreground, const QColor& background) {
    const double lf = relativeLuminance(foreground);
    const double lb = relativeLuminance(background);
    const double lighter = std::max(lf, lb);
    const double darker = std::min(lf, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

std::vector<QString> ThemePalette::validateContrast() const {
    // 「文字／背景」配對清單：涵蓋呈現層實際會疊字的每一種背景。
    // WCAG AA 一般文字門檻是 4.5:1（大字級 3:1，本專案一律用嚴格門檻）。
    const std::pair<ColorRole, ColorRole> pairs[] = {
        {ColorRole::WindowText, ColorRole::WindowBackground},
        {ColorRole::WindowText, ColorRole::Base},
        {ColorRole::WindowText, ColorRole::AlternateBase},
        {ColorRole::ButtonText, ColorRole::Button},
        {ColorRole::HighlightedText, ColorRole::Highlight},
        {ColorRole::Link, ColorRole::WindowBackground},
        {ColorRole::ToolTipText, ColorRole::ToolTipBase},
    };
    constexpr double kMinRatio = 4.5;
    std::vector<QString> failures;
    for (const auto& [fg, bg] : pairs) {
        const double ratio = contrastRatio(color(fg), color(bg));
        if (ratio < kMinRatio) {
            failures.push_back(QStringLiteral("%1 / %2 = %3:1（未達 4.5:1）")
                                    .arg(toString(fg), toString(bg))
                                    .arg(ratio, 0, 'f', 2));
        }
    }
    return failures;
}

}  // namespace alioth::app
