// 可調整游標大小（PRD-UI-018）。
//
// 系統游標交給作業系統縮放，我們只要驗證：等級 -> 倍率的對照表、與
// QSettings 之間往返不失真、自畫游標的圖樣大小與熱點確實依 DPI 縮放。

#include <QtTest>

#include <cmath>

#include "app/uisystem/cursor_scale.h"

using namespace alioth::app;

class TestCursorScale : public QObject {
    Q_OBJECT

private slots:
    void scaleFactorIncreasesMonotonicallyWithLevel() {
        QCOMPARE(CursorScale::scaleFactor(CursorSizeLevel::Normal), 1.0);
        QVERIFY(CursorScale::scaleFactor(CursorSizeLevel::Large) >
                CursorScale::scaleFactor(CursorSizeLevel::Normal));
        QVERIFY(CursorScale::scaleFactor(CursorSizeLevel::ExtraLarge) >
                CursorScale::scaleFactor(CursorSizeLevel::Large));
        QVERIFY(CursorScale::scaleFactor(CursorSizeLevel::Huge) >
                CursorScale::scaleFactor(CursorSizeLevel::ExtraLarge));
    }

    void settingsValueRoundTripsForAllLevels() {
        const CursorSizeLevel levels[] = {CursorSizeLevel::Normal, CursorSizeLevel::Large,
                                          CursorSizeLevel::ExtraLarge, CursorSizeLevel::Huge};
        for (const CursorSizeLevel level : levels) {
            const int value = CursorScale::toSettingsValue(level);
            QCOMPARE(CursorScale::fromSettingsValue(value), level);
        }
    }

    void invalidSettingsValueFallsBackToNormal() {
        // 設定檔被手動改壞或跨版本欄位增減時，讀取不該直接崩潰或給出未定義大小。
        QCOMPARE(CursorScale::fromSettingsValue(-1), CursorSizeLevel::Normal);
        QCOMPARE(CursorScale::fromSettingsValue(99), CursorSizeLevel::Normal);
    }

    void crosshairCursorGrowsWithLevelAndDevicePixelRatio() {
        const QCursor normal = CursorScale::buildCrosshairCursor(CursorSizeLevel::Normal, 1.0);
        const QCursor huge = CursorScale::buildCrosshairCursor(CursorSizeLevel::Huge, 1.0);
        QVERIFY(huge.pixmap().width() > normal.pixmap().width());

        const QCursor hiDpi = CursorScale::buildCrosshairCursor(CursorSizeLevel::Normal, 2.0);
        // devicePixelRatio 提高時，實體像素應該變大，但邏輯尺寸（width() /
        // devicePixelRatio()）要維持不變，否則游標在高 DPI 螢幕上會变得
        // 過大或過小。
        QVERIFY(hiDpi.pixmap().width() > normal.pixmap().width());
        QCOMPARE(hiDpi.pixmap().devicePixelRatio(), 2.0);
    }

    void hotspotStaysAtCenterAcrossSizes() {
        // 熱點必須精準對到圖樣中央，否則形狀工具畫出來的位置會跟游標尖端
        // 系統性地錯開。以邏輯像素比對：hotspot 應該接近 pixmap 邏輯寬度的一半。
        for (const CursorSizeLevel level :
             {CursorSizeLevel::Normal, CursorSizeLevel::Large, CursorSizeLevel::Huge}) {
            const QCursor cursor = CursorScale::buildCrosshairCursor(level, 1.5);
            const double logicalWidth = cursor.pixmap().width() / cursor.pixmap().devicePixelRatio();
            QVERIFY(std::abs(cursor.hotSpot().x() - logicalWidth / 2.0) <= 1.0);
            QVERIFY(std::abs(cursor.hotSpot().y() - logicalWidth / 2.0) <= 1.0);
        }
    }
};

// QPixmap/QCursor 需要 QGuiApplication，不能用 QTEST_GUILESS_MAIN——
// 這一支測試因此要在 offscreen 平台跑（見 tests/uisystem/CMakeLists.txt）。
QTEST_MAIN(TestCursorScale)
#include "test_cursor_scale.moc"
