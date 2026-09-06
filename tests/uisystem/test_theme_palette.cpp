// 主題色票測試（PRD-UI-006）。重點是 WCAG AA 對比度驗證，不是色票好不好看。

#include <QtTest>

#include "app/uisystem/theme_palette.h"

using namespace alioth::app;

class TestThemePalette : public QObject {
    Q_OBJECT

private slots:
    void lightPaletteMeetsWcagAA() {
        const auto failures = ThemePalette::light().validateContrast();
        for (const auto& f : failures) qWarning() << f;
        QVERIFY2(failures.empty(), "淺色主題有配對未達 WCAG AA 4.5:1");
    }

    void darkPaletteMeetsWcagAA() {
        const auto failures = ThemePalette::dark().validateContrast();
        for (const auto& f : failures) qWarning() << f;
        QVERIFY2(failures.empty(), "深色主題有配對未達 WCAG AA 4.5:1");
    }

    void contrastRatioIsSymmetric() {
        const double a = ThemePalette::contrastRatio(QColor(0, 0, 0), QColor(255, 255, 255));
        const double b = ThemePalette::contrastRatio(QColor(255, 255, 255), QColor(0, 0, 0));
        QCOMPARE(a, b);
    }

    void blackOnWhiteIsMaximumContrast() {
        const double ratio = ThemePalette::contrastRatio(QColor(0, 0, 0), QColor(255, 255, 255));
        QVERIFY(ratio > 20.0);  // 理論值 21:1
    }

    void sameColorHasRatioOne() {
        const double ratio = ThemePalette::contrastRatio(QColor(100, 100, 100), QColor(100, 100, 100));
        QCOMPARE(ratio, 1.0);
    }

    void forModeResolvesCorrectPalette() {
        QCOMPARE(ThemePalette::forMode(ThemeMode::Light).color(ColorRole::WindowBackground),
                 ThemePalette::light().color(ColorRole::WindowBackground));
        QCOMPARE(ThemePalette::forMode(ThemeMode::Dark).color(ColorRole::WindowBackground),
                 ThemePalette::dark().color(ColorRole::WindowBackground));
    }

    void lightAndDarkPalettesAreActuallyDifferent() {
        QVERIFY(ThemePalette::light().color(ColorRole::WindowBackground) !=
                ThemePalette::dark().color(ColorRole::WindowBackground));
    }

    void toQPaletteCarriesHighlightColors() {
        const QPalette pal = ThemePalette::light().toQPalette();
        QCOMPARE(pal.color(QPalette::Highlight), ThemePalette::light().color(ColorRole::Highlight));
    }
};

QTEST_GUILESS_MAIN(TestThemePalette)
#include "test_theme_palette.moc"
