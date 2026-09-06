#include "app/uisystem/theme_controller.h"

#include <QGuiApplication>
#include <QStyleHints>

namespace alioth::app {

ThemeController::ThemeController(QObject* parent) : QObject(parent) {
    if (qApp) {
        systemIsDark_ = qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme scheme) {
                    systemIsDark_ = (scheme == Qt::ColorScheme::Dark);
                    recomputeResolvedMode();
                });
    }
    recomputeResolvedMode();
}

void ThemeController::setMode(ThemeMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    recomputeResolvedMode();
}

void ThemeController::setSystemDarkModeForTesting(bool dark) {
    systemIsDark_ = dark;
    recomputeResolvedMode();
}

void ThemeController::recomputeResolvedMode() {
    const ThemeMode resolved = [&] {
        switch (mode_) {
            case ThemeMode::Light: return ThemeMode::Light;
            case ThemeMode::Dark: return ThemeMode::Dark;
            case ThemeMode::System: return systemIsDark_ ? ThemeMode::Dark : ThemeMode::Light;
        }
        return ThemeMode::Light;
    }();
    if (resolved == resolvedMode_) return;
    resolvedMode_ = resolved;
    emit themeChanged(resolvedMode_);
}

}  // namespace alioth::app
