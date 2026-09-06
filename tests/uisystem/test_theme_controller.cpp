// 主題控制器測試：跟隨系統模式下，系統色彩方案變化要真的反映到 resolvedMode()
// 並發出 themeChanged（PRD-UI-006）。用 setSystemDarkModeForTesting 注入變化，
// 不依賴真的作業系統事件——那樣測試才能在 offscreen CI 環境穩定跑。

#include <QtTest>
#include <QGuiApplication>

#include "app/uisystem/theme_controller.h"

using namespace alioth::app;

class TestThemeController : public QObject {
    Q_OBJECT

private slots:
    void explicitModeIgnoresSystem() {
        ThemeController controller;
        controller.setMode(ThemeMode::Dark);
        controller.setSystemDarkModeForTesting(false);
        QCOMPARE(controller.resolvedMode(), ThemeMode::Dark);
    }

    void systemModeFollowsInjectedSystemState() {
        ThemeController controller;
        controller.setMode(ThemeMode::System);
        controller.setSystemDarkModeForTesting(true);
        QCOMPARE(controller.resolvedMode(), ThemeMode::Dark);
        controller.setSystemDarkModeForTesting(false);
        QCOMPARE(controller.resolvedMode(), ThemeMode::Light);
    }

    void themeChangedEmittedOnActualChange() {
        ThemeController controller;
        controller.setMode(ThemeMode::System);
        controller.setSystemDarkModeForTesting(false);  // 確保起點是 Light

        QSignalSpy spy(&controller, &ThemeController::themeChanged);
        controller.setSystemDarkModeForTesting(true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<ThemeMode>(), ThemeMode::Dark);
    }

    void themeChangedNotEmittedWhenResolvedModeUnchanged() {
        ThemeController controller;
        controller.setMode(ThemeMode::Light);

        QSignalSpy spy(&controller, &ThemeController::themeChanged);
        controller.setSystemDarkModeForTesting(true);  // Light 模式下系統變化不該有影響
        QCOMPARE(spy.count(), 0);
        QCOMPARE(controller.resolvedMode(), ThemeMode::Light);
    }

    void currentPaletteMatchesResolvedMode() {
        ThemeController controller;
        controller.setMode(ThemeMode::Dark);
        QCOMPARE(controller.currentPalette().color(ColorRole::WindowBackground),
                 ThemePalette::dark().color(ColorRole::WindowBackground));
    }
};

QTEST_MAIN(TestThemeController)
#include "test_theme_controller.moc"
