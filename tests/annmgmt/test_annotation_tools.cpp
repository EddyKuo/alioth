// WP28 純邏輯核心測試:鉛筆平滑化、橡皮擦、對齊/微調/貼上位移、旋轉、樣式、旗標。
//
// 全部是領域層純函數,不需要 QApplication 事件迴圈,也不需要 PDFium。

#include <QtTest>

#include <cmath>

#include "domain/annotation.h"
#include "domain/annotation_eraser.h"
#include "domain/annotation_flags_ops.h"
#include "domain/annotation_layout_tools.h"
#include "domain/annotation_rotation.h"
#include "domain/annotation_style.h"
#include "domain/ink_smoothing.h"

using namespace alioth::domain;

namespace {

double distance(const PointF& a, const PointF& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

}  // namespace

class TestAnnotationTools : public QObject {
    Q_OBJECT

private slots:
    // PRD-ANN-003:平滑化不得把原始筆跡改到認不出來——最大偏差恆有上限。
    void smoothingRespectsMaxDeviationBound() {
        std::vector<PressurePoint> jagged;
        for (int i = 0; i < 30; ++i) {
            // 鋸齒抖動:偶數點往上偏,奇數點往下偏,製造需要被平滑掉的雜訊。
            const double x = static_cast<double>(i) * 5.0;
            const double y = (i % 2 == 0) ? 10.0 : -10.0;
            jagged.push_back(PressurePoint{PointF{x, y}, 0.5});
        }

        SmoothingOptions options{};
        options.windowRadius = 2;
        options.maxDeviation = 3.0;

        const std::vector<PressurePoint> smoothed = smoothStroke(jagged, options);
        QCOMPARE(smoothed.size(), jagged.size());

        for (std::size_t i = 0; i < smoothed.size(); ++i) {
            const double d = distance(smoothed[i].position, jagged[i].position);
            QVERIFY2(d <= options.maxDeviation + 1e-9,
                     qPrintable(QStringLiteral("點 %1 偏差 %2 超過上限").arg(i).arg(d)));
            // 壓力值不受平滑影響。
            QCOMPARE(smoothed[i].pressure, jagged[i].pressure);
        }

        // 端點恆不平滑,是使用者落筆/收筆位置的一部分。
        QCOMPARE(smoothed.front().position, jagged.front().position);
        QCOMPARE(smoothed.back().position, jagged.back().position);
    }

    void smoothingWithZeroRadiusIsIdentity() {
        std::vector<PointF> points{{0, 0}, {1, 5}, {2, 0}, {3, 5}};
        SmoothingOptions options{};
        options.windowRadius = 0;
        const std::vector<PointF> out = smoothStrokePositions(points, options);
        QCOMPARE(out, points);
    }

    // PRD-ANN-020:橡皮擦只挖掉半徑內的點,一條筆畫中間被挖空要斷成兩段。
    void eraserSplitsStrokeWhenMiddleIsRemoved() {
        InkGeometry ink;
        std::vector<PointF> stroke;
        for (int i = 0; i <= 20; ++i) stroke.push_back(PointF{static_cast<double>(i), 0.0});
        ink.strokes.push_back(stroke);

        std::vector<PointF> eraserPath{{10.0, 0.0}};
        EraseOptions options{};
        options.radius = 1.5;
        options.minRemainingPoints = 2;

        const InkGeometry erased = eraseFromInk(ink, eraserPath, options);
        QCOMPARE(erased.strokes.size(), std::size_t{2});
        // 兩段各自都完全落在擦除點的兩側。
        for (const PointF& p : erased.strokes[0]) QVERIFY(p.x < 10.0 - options.radius);
        for (const PointF& p : erased.strokes[1]) QVERIFY(p.x > 10.0 + options.radius);
    }

    void eraserRemovingWholeStrokeYieldsEmpty() {
        InkGeometry ink;
        ink.strokes.push_back({{0, 0}, {1, 0}, {2, 0}});
        std::vector<PointF> eraserPath{{0, 0}, {1, 0}, {2, 0}};
        EraseOptions options{};
        options.radius = 5.0;
        const InkGeometry erased = eraseFromInk(ink, eraserPath, options);
        QVERIFY(isFullyErased(erased));
    }

    // PRD-ANN-011:對齊、分散、鍵盤微調、貼上位移。
    void alignLeftMovesUnlockedToMinLeft() {
        std::vector<SelectableRect> items = {
            SelectableRect{RectF{10, 0, 30, 10}, false},
            SelectableRect{RectF{50, 0, 70, 10}, false},
            SelectableRect{RectF{5, 0, 25, 10}, false},
        };
        const std::vector<RectF> out = alignSelection(items, AlignEdge::Left);
        for (const RectF& r : out) QCOMPARE(r.left, 5.0);
    }

    void alignSkipsLockedItems() {
        std::vector<SelectableRect> items = {
            SelectableRect{RectF{10, 0, 30, 10}, true},
            SelectableRect{RectF{50, 0, 70, 10}, false},
        };
        const std::vector<RectF> out = alignSelection(items, AlignEdge::Left);
        QCOMPARE(out[0], items[0].rect);  // 鎖定項目原封不動
        QCOMPARE(out[1].left, 50.0);      // 基準線只由未鎖定項目決定(它自己)
    }

    void distributeHorizontalIsEvenlySpaced() {
        std::vector<SelectableRect> items = {
            SelectableRect{RectF{0, 0, 10, 10}, false},
            SelectableRect{RectF{12, 0, 15, 10}, false},
            SelectableRect{RectF{40, 0, 50, 10}, false},
            SelectableRect{RectF{100, 0, 110, 10}, false},
        };
        const std::vector<RectF> out = distributeSelection(items, DistributeAxis::Horizontal);
        const double c0 = (out[0].left + out[0].right) / 2.0;
        const double c1 = (out[1].left + out[1].right) / 2.0;
        const double c2 = (out[2].left + out[2].right) / 2.0;
        const double c3 = (out[3].left + out[3].right) / 2.0;
        QVERIFY(std::abs((c1 - c0) - (c2 - c1)) < 1e-6);
        QVERIFY(std::abs((c2 - c1) - (c3 - c2)) < 1e-6);
        QCOMPARE(out[0], items[0].rect);  // 首尾維持原位
        QCOMPARE(out[3], items[3].rect);
    }

    void nudgeMovesOnlyUnlocked() {
        std::vector<SelectableRect> items = {
            SelectableRect{RectF{0, 0, 10, 10}, false},
            SelectableRect{RectF{0, 0, 10, 10}, true},
        };
        const std::vector<RectF> out = nudgeSelection(items, 5.0, -3.0);
        QCOMPARE(out[0], RectF(5.0, -3.0, 15.0, 7.0));
        QCOMPARE(out[1], items[1].rect);
    }

    void pasteOffsetSamePageDoesNotOverlapExactly() {
        const RectF source{10, 10, 30, 30};
        const RectF pasted = pasteOffsetSamePage(source);
        QVERIFY(pasted != source);
    }

    void pasteOffsetForPageAnchorsToTopLeft() {
        const RectF source{0, 780, 100, 800};  // A4 頁 (595x842) 上緣附近
        const SizeF sourcePage{595, 842};
        const SizeF targetPage{595, 842};
        const RectF pasted = pasteOffsetForPage(source, sourcePage, targetPage);
        // 同尺寸頁面貼上時位置應完全不變。
        QCOMPARE(pasted, source);
    }

    // PRD-ANN-023:旋轉,Shift 吸附至 15 度增量。
    void snapRoundsToNearestFifteen() {
        QCOMPARE(snapToFifteenDegrees(7.0), 0.0);
        QCOMPARE(snapToFifteenDegrees(8.0), 15.0);
        QCOMPARE(snapToFifteenDegrees(22.0), 15.0);
        QCOMPARE(snapToFifteenDegrees(23.0), 30.0);
    }

    void rotateAnnotationRotatesLineAroundCenter() {
        Annotation annotation;
        annotation.rect = RectF{0, 0, 10, 0};
        LineGeometry line;
        line.start = PointF{0, 0};
        line.end = PointF{10, 0};
        annotation.geometry = line;

        const Annotation rotated = rotateAnnotation(annotation, 90.0, false);
        const auto* rotatedLine = std::get_if<LineGeometry>(&rotated.geometry);
        QVERIFY(rotatedLine != nullptr);
        // 繞 (5,0) 轉 90 度: (0,0) -> (5,-5), (10,0) -> (5,5)
        QVERIFY(std::abs(rotatedLine->start.x - 5.0) < 1e-9);
        QVERIFY(std::abs(rotatedLine->start.y - (-5.0)) < 1e-9);
        QVERIFY(std::abs(rotatedLine->end.x - 5.0) < 1e-9);
        QVERIFY(std::abs(rotatedLine->end.y - 5.0) < 1e-9);
    }

    void rotateAnnotationSnapsWhenShiftHeld() {
        Annotation annotation;
        annotation.rect = RectF{0, 0, 10, 0};
        LineGeometry line;
        line.start = PointF{0, 0};
        line.end = PointF{10, 0};
        annotation.geometry = line;

        const Annotation exact = rotateAnnotation(annotation, 92.0, false);
        const Annotation snapped = rotateAnnotation(annotation, 92.0, true);  // 吸附到 90
        const auto* exactLine = std::get_if<LineGeometry>(&exact.geometry);
        const auto* snappedLine = std::get_if<LineGeometry>(&snapped.geometry);
        QVERIFY(exactLine != nullptr && snappedLine != nullptr);
        QVERIFY(std::abs(snappedLine->end.y - 5.0) < 1e-9);
        QVERIFY(std::abs(exactLine->end.y - 5.0) > 1e-6);  // 未吸附時不會剛好落在整數角
    }

    // PRD-ANN-029:樣式套用不動內容/位置/旗標。
    void applyStyleOnlyTouchesVisualFields() {
        Annotation annotation;
        annotation.contents = "審閱意見";
        annotation.author = "Alice";
        annotation.rect = RectF{0, 0, 10, 10};
        annotation.flags = AnnotationFlag::Print | AnnotationFlag::Locked;
        annotation.color = ColorRgb{0, 0, 0};

        CommentStyle style;
        style.color = ColorRgb{1, 0, 0};
        style.opacity = 0.5;

        const Annotation styled = applyStyle(annotation, style);
        QCOMPARE(styled.contents, annotation.contents);
        QCOMPARE(styled.author, annotation.author);
        QCOMPARE(styled.rect, annotation.rect);
        QCOMPARE(styled.flags, annotation.flags);
        QCOMPARE(styled.color, style.color);
        QCOMPARE(styled.opacity, style.opacity);
        QVERIFY(hasStyle(styled, style));
    }

    void extractStyleRoundTrips() {
        Annotation annotation;
        annotation.color = ColorRgb{0.2, 0.4, 0.6};
        annotation.interiorColor = ColorRgb{0.1, 0.1, 0.1};
        annotation.opacity = 0.75;
        annotation.border.width = 2.0;

        const CommentStyle style = extractStyle(annotation);
        const Annotation reapplied = applyStyle(annotation, style);
        QVERIFY(hasStyle(reapplied, style));
    }

    // PRD-ANN-012:鎖定/隱藏/列印旗標操作。
    void lockSetsBothLockFlags() {
        Annotation annotation;
        annotation.flags = AnnotationFlag::Print;
        const Annotation locked = setLocked(annotation, true);
        QVERIFY(isLocked(locked));
        QVERIFY(hasFlag(locked.flags, AnnotationFlag::LockedContents));
        QVERIFY(isPrintable(locked));  // 鎖定不影響列印旗標

        const Annotation unlocked = setLocked(locked, false);
        QVERIFY(!isLocked(unlocked));
        QVERIFY(!hasFlag(unlocked.flags, AnnotationFlag::LockedContents));
    }

    void hidingClearsPrintFlag() {
        Annotation annotation;
        annotation.flags = AnnotationFlag::Print;
        const Annotation hidden = setHidden(annotation, true);
        QVERIFY(isHidden(hidden));
        QVERIFY(!isPrintable(hidden));

        const Annotation shown = setHidden(hidden, false);
        QVERIFY(!isHidden(shown));
    }

    void printableToggle() {
        Annotation annotation;
        annotation.flags = AnnotationFlag::None;
        const Annotation printable = setPrintable(annotation, true);
        QVERIFY(isPrintable(printable));
        const Annotation notPrintable = setPrintable(printable, false);
        QVERIFY(!isPrintable(notPrintable));
    }
};

QTEST_APPLESS_MAIN(TestAnnotationTools)
#include "test_annotation_tools.moc"
