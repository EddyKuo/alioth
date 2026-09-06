// PRD-ZOOM-004（區域框選縮放、Loupe）、PRD-ZOOM-005（Pan & Zoom 面板）。
//
// 三個函式都刻意不碰渲染——它們只算「該用什麼倍率、該請求哪一小塊」，
// 實際圖磚仍然走既有管線（見 domain/zoom_aids.h 開頭說明）。這裡只驗算術。

#include <QtTest>

#include <cmath>

#include "domain/zoom_aids.h"

using namespace alioth::domain;

namespace {
bool nearlyEqual(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

class TestZoomAids : public QObject {
    Q_OBJECT

private slots:
    void rectZoomFillsViewport() {
        RectZoomRequest request;
        request.selection = RectI{100, 100, 400, 300};  // 4:3
        request.currentScale = 1.0;
        request.viewportPx = SizeF{800.0, 600.0};  // 也是 4:3，兩軸縮放比例相同
        request.minScale = 0.1;
        request.maxScale = 64.0;

        const auto result = computeRectZoom(request);
        QVERIFY(nearlyEqual(result.newScale, 2.0));
        // 框選中心 (300,250) 在新倍率下應該是 (600,500)。
        QVERIFY(nearlyEqual(result.centerAtNewScale.x, 600.0));
        QVERIFY(nearlyEqual(result.centerAtNewScale.y, 500.0));
    }

    void rectZoomClampsToMaxScale() {
        RectZoomRequest request;
        request.selection = RectI{0, 0, 10, 10};
        request.currentScale = 1.0;
        request.viewportPx = SizeF{1000.0, 1000.0};
        request.minScale = 0.1;
        request.maxScale = 8.0;
        const auto result = computeRectZoom(request);
        QCOMPARE(result.newScale, 8.0);
    }

    void rectZoomIgnoresDegenerateSelection() {
        RectZoomRequest request;
        request.selection = RectI{10, 10, 0, 0};
        request.currentScale = 1.5;
        request.viewportPx = SizeF{800.0, 600.0};
        const auto result = computeRectZoom(request);
        QCOMPARE(result.newScale, 1.5);
    }

    void loupeSampleCentersOnCursorAtEffectiveScale() {
        LoupeRequest request;
        request.cursorAtBaseScale = PointF{200.0, 150.0};
        request.baseScale = 1.0;
        request.magnification = 3.0;
        request.loupeWidgetPx = SizeF{120.0, 120.0};

        const auto sample = computeLoupeSample(request);
        QVERIFY(nearlyEqual(sample.effectiveScale, 3.0));
        // 游標在有效倍率下是 (600,450)，樣本矩形應以它為中心，120x120。
        QCOMPARE(sample.sampleRect.width, 120);
        QCOMPARE(sample.sampleRect.height, 120);
        QCOMPARE(sample.sampleRect.x, 600 - 60);
        QCOMPARE(sample.sampleRect.y, 450 - 60);
    }

    // Pan & Zoom 面板：主視圖可視區 → 縮圖矩形 → 主視圖原點，必須互為逆運算，
    // 否則面板拖一下主視圖跳到別的地方。
    void panZoomRoundTrip() {
        const SizeF contentPx{2000.0, 3000.0};
        const SizeF panelPx{200.0, 300.0};
        const double thumbScale = panZoomThumbnailScale(contentPx, panelPx);
        QVERIFY(nearlyEqual(thumbScale, 0.1));

        const double mainScale = 1.4;
        const RectI mainViewport{357, 921, 640, 480};

        const auto thumbRect = mainViewportToThumbnailRect(mainViewport, mainScale, thumbScale);
        const PointF recoveredOrigin = thumbnailRectToMainOrigin(thumbRect, mainScale, thumbScale);

        QVERIFY(nearlyEqual(recoveredOrigin.x, static_cast<double>(mainViewport.x), 1e-6));
        QVERIFY(nearlyEqual(recoveredOrigin.y, static_cast<double>(mainViewport.y), 1e-6));
    }
};

QTEST_MAIN(TestZoomAids)
#include "test_zoom_aids.moc"
