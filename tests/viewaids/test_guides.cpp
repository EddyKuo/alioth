// PRD-VIEW-015（尺規與參考線）、PRD-VIEW-016（格線貼齊）。
//
// 判準是座標往返自洽：參考線與貼齊都活在頁面空間，畫到螢幕上要經
// PageTransform；只要那個轉換在任何縮放/旋轉下往返無漂移，貼齊算出來的
// 頁面座標换回裝置座標時就不會偏移。這裡除了直接測貼齊函式，也顯式測了
// 一次「尺規拖出參考線」的完整往返（裝置座標 → 頁面座標存成參考線 →
// 裝置座標），涵蓋四種旋轉。

#include <QtTest>

#include <cmath>

#include "domain/guides.h"

using namespace alioth::domain;

namespace {
bool nearlyEqual(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

class TestGuides : public QObject {
    Q_OBJECT

private slots:
    void snapToGridWithinTolerance() {
        GridSettings grid{true, 10.0};
        const auto result = snapToGrid(23.5, grid, 5.0);
        QVERIFY(result.snapped);
        QCOMPARE(result.source, SnapSource::Grid);
        QVERIFY(nearlyEqual(result.value, 20.0));
    }

    void snapToGridOutsideToleranceIsNoop() {
        GridSettings grid{true, 10.0};
        const auto result = snapToGrid(23.5, grid, 1.0);
        QVERIFY(!result.snapped);
        QVERIFY(nearlyEqual(result.value, 23.5));
    }

    void disabledGridNeverSnaps() {
        GridSettings grid{false, 10.0};
        const auto result = snapToGrid(20.0, grid, 5.0);
        QVERIFY(!result.snapped);
    }

    void guidesTakePriorityOverGrid() {
        GuideSet guides;
        guides.add(GuideLine{GuideOrientation::Vertical, 21.0, false});
        GridSettings grid{true, 10.0};
        // 兩者都在容差內：格線最近的是 20.0，參考線是 21.0；參考線應該贏。
        const auto result = snapAxis(21.5, GuideOrientation::Vertical, guides, grid, {}, 5.0);
        QVERIFY(result.snapped);
        QCOMPARE(result.source, SnapSource::Guide);
        QVERIFY(nearlyEqual(result.value, 21.0));
    }

    void lockedGuideIgnoresMove() {
        GuideSet guides;
        const auto index = guides.add(GuideLine{GuideOrientation::Horizontal, 100.0, true});
        guides.move(index, 200.0);
        QVERIFY(nearlyEqual(guides.lines()[static_cast<std::size_t>(index)].positionPt, 100.0));
    }

    void objectEdgeSnapBeatsGrid() {
        GridSettings grid{true, 10.0};
        const std::vector<double> edges{17.0};
        const auto result = snapAxis(18.0, GuideOrientation::Horizontal, GuideSet{}, grid, edges, 5.0);
        QCOMPARE(result.source, SnapSource::ObjectEdge);
        QVERIFY(nearlyEqual(result.value, 17.0));
    }

    // 尺規拖出參考線的完整往返：使用者在裝置座標按下、放開，換算出的頁面座標
    // 存成參考線；重新畫面時再由 PageTransform 換回裝置座標，必須拿回原本的
    // 拖曳位置（誤差 < 半像素），四種旋轉都要成立。
    void guideRoundTripAcrossRotations_data() {
        QTest::addColumn<int>("rotation");
        for (int r = 0; r < 4; ++r) {
            QTest::addRow("rotation-%d", r) << r;
        }
    }

    void guideRoundTripAcrossRotations() {
        QFETCH(int, rotation);
        const PageTransform transform(SizeF{612.0, 792.0}, 1.75,
                                      static_cast<Rotation>(rotation));

        // 使用者在裝置座標上拖出一條垂直參考線（水平位置）。
        const PointF devicePress{123.0, 456.0};
        const PointF pagePoint = transform.toPage(devicePress);

        GuideSet guides;
        guides.add(GuideLine{GuideOrientation::Vertical, pagePoint.x, false});

        // 重畫：把參考線的頁面座標換回裝置座標——用一個高度與 press 相同的點，
        // 只驗 x 軸（垂直參考線本來就只固定一軸）。
        const PointF backToDevice = transform.toDevice({guides.lines()[0].positionPt, pagePoint.y});
        QVERIFY(nearlyEqual(backToDevice.x, devicePress.x, 0.5));
        QVERIFY(nearlyEqual(backToDevice.y, devicePress.y, 0.5));
    }
};

QTEST_MAIN(TestGuides)
#include "test_guides.moc"
