// 觸控最佳化的純邏輯測試（PRD-UI-013）。
//
// 重點放在最容易做錯的一點：位移超過門檻就不該再算長按，而要讓呼叫端當成
// 拖曳處理。時間門檻與位移門檻分開測，確保兩個條件是「且」不是「或」。

#include <QtTest>

#include <cmath>

#include "app/touch/touch_gestures.h"

using namespace alioth::app;

class TestTouchGestures : public QObject {
    Q_OBJECT

private slots:
    // --- TouchTargetMetrics ---

    void minimumTargetSizeScalesWithDpi() {
        // 96 DPI 是 CSS px 的參考基準：44 CSS px 應該原封不動地等於 44 裝置獨立像素。
        QCOMPARE(TouchTargetMetrics::minimumTargetSizePx(96.0), 44.0);
        // 192 DPI（常見的 200% 縮放）應該剛好是兩倍。
        QCOMPARE(TouchTargetMetrics::minimumTargetSizePx(192.0), 88.0);
    }

    void minimumTargetSizeFallsBackToReferenceWhenDpiInvalid() {
        // DPI 讀不到（0 或負值）不該讓計算結果變成 0 或負的荒謬尺寸。
        QCOMPARE(TouchTargetMetrics::minimumTargetSizePx(0.0), 44.0);
        QCOMPARE(TouchTargetMetrics::minimumTargetSizePx(-10.0), 44.0);
    }

    void meetsMinimumTargetRejectsTooSmallCandidates() {
        QVERIFY(TouchTargetMetrics::meetsMinimumTarget(QSizeF(44.0, 44.0), 96.0));
        QVERIFY(!TouchTargetMetrics::meetsMinimumTarget(QSizeF(43.9, 44.0), 96.0));
        QVERIFY(!TouchTargetMetrics::meetsMinimumTarget(QSizeF(44.0, 20.0), 96.0));
    }

    // --- LongPressGesture ---

    void firesExactlyOnceAfterDurationWithoutMovement() {
        LongPressGesture gesture({/*pressDurationMs=*/500, /*moveToleranceDevicePx=*/10.0});
        gesture.press(QPointF(100, 100), 0);

        QVERIFY(!gesture.checkTimeout(499));
        QVERIFY(gesture.checkTimeout(500));
        // 同一次按壓不會再觸發第二次——上層不該因為連續輪詢就開兩次選單。
        QVERIFY(!gesture.checkTimeout(600));
    }

    void movementBeyondToleranceCancelsLongPressPermanently() {
        // 這是最容易做錯、也最惱人的一點：位移超過門檻就不算長按，是拖曳。
        LongPressGesture gesture({500, 10.0});
        gesture.press(QPointF(0, 0), 0);
        gesture.move(QPointF(20, 0), 100);  // 位移 20px > 10px 門檻

        QVERIFY(!gesture.checkTimeout(500));
        QVERIFY(!gesture.checkTimeout(1000));
    }

    void movementBackToOriginDoesNotRestoreEligibility() {
        // 手指滑出容許範圍之後又滑回原點，不代表使用者「改變心意想長按」了——
        // 一旦逾越門檻，這次手勢就永久出局。
        LongPressGesture gesture({500, 10.0});
        gesture.press(QPointF(0, 0), 0);
        gesture.move(QPointF(50, 0), 50);
        gesture.move(QPointF(0, 0), 100);

        QVERIFY(!gesture.checkTimeout(500));
    }

    void movementWithinToleranceStillFires() {
        // 觸控手指本來就不可能完全靜止，容許誤差內的抖動不該取消長按。
        LongPressGesture gesture({500, 10.0});
        gesture.press(QPointF(0, 0), 0);
        gesture.move(QPointF(5, 3), 200);

        QVERIFY(gesture.checkTimeout(500));
    }

    void releaseBeforeTimeoutCancelsLongPress() {
        LongPressGesture gesture({500, 10.0});
        gesture.press(QPointF(0, 0), 0);
        gesture.release();

        QVERIFY(!gesture.checkTimeout(500));
    }

    void secondPressResetsPreviousState() {
        LongPressGesture gesture({500, 10.0});
        gesture.press(QPointF(0, 0), 0);
        gesture.move(QPointF(100, 0), 50);  // 取消資格
        QVERIFY(!gesture.checkTimeout(500));

        // 新的一次按壓應該完全獨立，不被上一次的取消狀態污染。
        gesture.press(QPointF(200, 200), 1000);
        QVERIFY(gesture.checkTimeout(1500));
    }

    // --- PinchZoomTracker ---

    void pinchScaleRatioMatchesDistanceRatio() {
        PinchZoomTracker tracker;
        tracker.begin(QPointF(0, 0), QPointF(100, 0), /*startScale=*/1.0);

        // 距離從 100 變成 200，倍率應該加倍。
        const auto update = tracker.update(QPointF(0, 0), QPointF(200, 0));
        QCOMPARE(update.scale, 2.0);
    }

    void pinchAnchorIsMidpointOfCurrentFingers() {
        PinchZoomTracker tracker;
        tracker.begin(QPointF(0, 0), QPointF(100, 0), 1.0);

        const auto update = tracker.update(QPointF(10, 20), QPointF(50, 20));
        QCOMPARE(update.anchor, QPointF(30, 20));
    }

    void pinchScaleAppliesOnTopOfStartingScale() {
        PinchZoomTracker tracker;
        tracker.begin(QPointF(0, 0), QPointF(100, 0), /*startScale=*/2.0);

        // 距離沒變，倍率應該維持在手勢開始時的倍率，不是被重置成 1.0。
        const auto update = tracker.update(QPointF(0, 0), QPointF(100, 0));
        QCOMPARE(update.scale, 2.0);
    }

    void pinchHandlesNearZeroStartDistanceWithoutBlowingUp() {
        // 兩指幾乎重疊時的起始距離被夾了下限，不該產生 inf/NaN。
        PinchZoomTracker tracker;
        tracker.begin(QPointF(0, 0), QPointF(0, 0), 1.0);
        const auto update = tracker.update(QPointF(0, 0), QPointF(50, 0));
        QVERIFY(std::isfinite(update.scale));
        QVERIFY(update.scale > 0.0);
    }
};

QTEST_GUILESS_MAIN(TestTouchGestures)
#include "test_touch_gestures.moc"
