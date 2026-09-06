// PRD-VIEW-009：全螢幕／簡報模式狀態機與頁面轉場緩動。

#include <QtTest>

#include "domain/presentation.h"

using namespace alioth::domain;

class TestPresentation : public QObject {
    Q_OBJECT

private slots:
    void toggleFullscreenRoundTrip() {
        PresentationController controller;
        QCOMPARE(controller.mode(), ViewMode::Normal);
        controller.toggleFullscreen();
        QCOMPARE(controller.mode(), ViewMode::Fullscreen);
        controller.toggleFullscreen();
        QCOMPARE(controller.mode(), ViewMode::Normal);
    }

    void escapeOnlyLeavesFullscreenOrPresentation() {
        PresentationController controller;
        QVERIFY(!controller.handleEscape());  // Normal 模式下 Esc 沒作用
        QCOMPARE(controller.mode(), ViewMode::Normal);

        controller.toggleFullscreen();
        QVERIFY(controller.handleEscape());
        QCOMPARE(controller.mode(), ViewMode::Normal);
    }

    void presentationRemembersPreviousMode() {
        PresentationController controller;
        controller.toggleFullscreen();  // Normal -> Fullscreen
        controller.enterPresentation(); // Fullscreen -> Presentation
        QCOMPARE(controller.mode(), ViewMode::Presentation);
        QVERIFY(controller.handleEscape());
        // 退出簡報模式應該回到 Fullscreen，不是永遠跳回 Normal。
        QCOMPARE(controller.mode(), ViewMode::Fullscreen);
    }

    void transitionProgressEndpointsAndMonotonic() {
        QCOMPARE(transitionProgress(0.0, 200.0), 0.0);
        QCOMPARE(transitionProgress(200.0, 200.0), 1.0);
        QCOMPARE(transitionProgress(500.0, 200.0), 1.0);  // 超出時長仍夾在 1.0
        QVERIFY(transitionProgress(50.0, 200.0) < transitionProgress(150.0, 200.0));
    }

    void transitionProgressZeroDurationCompletesImmediately() {
        QCOMPARE(transitionProgress(0.0, 0.0), 1.0);
    }

    void slideOffsetsOppositeDirectionsHaveOppositeSigns() {
        const auto forward = slideOffsets(0.5, true);
        const auto backward = slideOffsets(0.5, false);
        QVERIFY(forward.outgoing < 0.0);
        QVERIFY(backward.outgoing > 0.0);
    }
};

QTEST_MAIN(TestPresentation)
#include "test_presentation.moc"
