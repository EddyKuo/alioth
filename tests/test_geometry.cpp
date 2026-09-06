// 座標轉換測試。
//
// 頁面座標 Y 向上、裝置座標 Y 向下，這個翻轉是註解標到錯位置的頭號原因，
// 所以四種旋轉都要驗來回轉換的自洽性。

#include <QtTest>

#include <cmath>

#include "domain/geometry.h"
#include "domain/tile.h"

using namespace alioth::domain;

namespace {

bool nearlyEqual(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) < eps;
}

}  // namespace

class TestGeometry : public QObject {
    Q_OBJECT

private slots:
    void roundTripAllRotations_data() {
        QTest::addColumn<int>("rotation");
        QTest::newRow("0") << 0;
        QTest::newRow("90") << 1;
        QTest::newRow("180") << 2;
        QTest::newRow("270") << 3;
    }

    void roundTripAllRotations() {
        QFETCH(int, rotation);
        const PageTransform transform(SizeF{595.0, 842.0}, 1.5,
                                      static_cast<Rotation>(rotation));

        for (const PointF& page : {PointF{0.0, 0.0}, PointF{595.0, 842.0}, PointF{100.0, 700.0},
                                   PointF{297.5, 421.0}}) {
            const PointF device = transform.toDevice(page);
            const PointF back = transform.toPage(device);
            QVERIFY2(nearlyEqual(back.x, page.x, 1e-6) && nearlyEqual(back.y, page.y, 1e-6),
                     qPrintable(QStringLiteral("往返不自洽: (%1,%2) -> (%3,%4) -> (%5,%6)")
                                    .arg(page.x).arg(page.y)
                                    .arg(device.x).arg(device.y)
                                    .arg(back.x).arg(back.y)));
        }
    }

    void pageOriginMapsToBottomLeft() {
        // 頁面原點在左下；未旋轉時它應該落在裝置空間的左下角，而不是左上角。
        const PageTransform transform(SizeF{100.0, 200.0}, 2.0, Rotation::None);
        const PointF device = transform.toDevice(PointF{0.0, 0.0});
        QCOMPARE(device.x, 0.0);
        QCOMPARE(device.y, 400.0);  // = 200pt * 2.0，即裝置空間底部
    }

    void rotationSwapsDeviceAxes() {
        const PageTransform portrait(SizeF{595.0, 842.0}, 1.0, Rotation::None);
        const PageTransform landscape(SizeF{595.0, 842.0}, 1.0, Rotation::Cw90);
        QCOMPARE(portrait.deviceSize().width, 595.0);
        QCOMPARE(portrait.deviceSize().height, 842.0);
        QCOMPARE(landscape.deviceSize().width, 842.0);
        QCOMPARE(landscape.deviceSize().height, 595.0);
    }

    void rectIntersection() {
        const RectF a{0.0, 0.0, 100.0, 100.0};
        const RectF b{50.0, 50.0, 150.0, 150.0};
        QVERIFY(a.intersects(b));
        const RectF hit = a.intersected(b);
        QCOMPARE(hit.left, 50.0);
        QCOMPARE(hit.top, 100.0);

        const RectF far{200.0, 200.0, 300.0, 300.0};
        QVERIFY(!a.intersects(far));
    }

    void scaleKeysAreExactAndComparable() {
        // 精確鍵：使用者要 1.3 倍就存 1.3 倍的圖磚，不是最接近階梯的那一格。
        QCOMPARE(exactScaleKey(1.0), 1000);
        QCOMPARE(exactScaleKey(1.3), 1300);
        QCOMPARE(scaleOfKey(exactScaleKey(2.5)), 2.5);

        // 浮點誤差不得造成不同的鍵，否則快取命中率會莫名其妙掉到零。
        QCOMPARE(exactScaleKey(0.1 + 0.2 + 1.0), exactScaleKey(1.3));
    }

    void coarseScaleKeysShareTilesDuringZooming() {
        // 過場用的粗略鍵：相鄰的中間倍率要落在同一格，整段縮放才共用同一批圖磚。
        QCOMPARE(coarseScaleKey(1.0), coarseScaleKey(1.02));
        QVERIFY(coarseScaleKey(1.0) != coarseScaleKey(2.0));

        // 單調遞增：倍率變大時鍵不得倒退。
        std::int32_t previous = coarseScaleKey(0.08);
        for (double scale = 0.08; scale <= 64.0; scale *= 1.03) {
            const std::int32_t key = coarseScaleKey(scale);
            QVERIFY(key >= previous);
            previous = key;
        }

        // 粗略鍵對應的倍率與實際倍率的落差不得超過階梯的一半，
        // 否則過場畫面的拉伸會明顯到像是壞掉。
        for (double scale : {0.25, 0.5, 1.0, 2.0, 8.0, 64.0}) {
            const double snapped = scaleOfKey(coarseScaleKey(scale));
            QVERIFY(std::abs(snapped - scale) / scale < 0.06);
        }
    }

    void tileKeysHashDistinctly() {
        const TileKey a{0, 0, 0, 0, Rotation::None, false};
        const TileKey b{0, 0, 0, 0, Rotation::None, true};
        const TileKey c{0, 0, 1, 0, Rotation::None, false};
        std::hash<TileKey> hasher;
        QVERIFY(a == a);
        QVERIFY(!(a == b));
        QVERIFY(hasher(a) != hasher(b));
        QVERIFY(hasher(a) != hasher(c));
    }
};

QTEST_APPLESS_MAIN(TestGeometry)
#include "test_geometry.moc"
