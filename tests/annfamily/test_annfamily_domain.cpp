// 領域層幾何測試（WP24）：PRD-ANN-018 Free Highlight 的手繪路徑轉換。
//
// 只測 domain::ribbonQuadsFromStroke——這是本包唯一新增的領域層純函數，
// 其餘型別（CaretGeometry、FreeTextGeometry）都是純資料結構，沒有邏輯可測。

#include <QtTest>

#include <cmath>

#include "domain/annotation.h"

using namespace alioth::domain;

class TestAnnfamilyDomain : public QObject {
    Q_OBJECT

private slots:
    void horizontalStrokeProducesAnAxisAlignedRibbon() {
        // 一段水平線，半寬 2：四個角必須精確落在 y = ±2 的兩條水平線上。
        const std::vector<PointF> points = {{0.0, 0.0}, {10.0, 0.0}};
        const auto quads = ribbonQuadsFromStroke(points, 2.0);
        QCOMPARE(quads.size(), std::size_t{1});
        const QuadPoint& q = quads.front();
        QCOMPARE(q.upperLeft.x, 0.0);
        QCOMPARE(q.upperLeft.y, 2.0);
        QCOMPARE(q.upperRight.x, 10.0);
        QCOMPARE(q.upperRight.y, 2.0);
        QCOMPARE(q.lowerLeft.x, 0.0);
        QCOMPARE(q.lowerLeft.y, -2.0);
        QCOMPARE(q.lowerRight.x, 10.0);
        QCOMPARE(q.lowerRight.y, -2.0);
    }

    void verticalStrokeProducesAnAxisAlignedRibbon() {
        // 垂直線：法線方向轉 90 度，寬度應該出現在 x 軸上而不是 y 軸上——
        // 這條測試專門抓「法線算成切線」這種寫反的錯誤。
        const std::vector<PointF> points = {{5.0, 0.0}, {5.0, 20.0}};
        const auto quads = ribbonQuadsFromStroke(points, 3.0);
        QCOMPARE(quads.size(), std::size_t{1});
        const QuadPoint& q = quads.front();
        // upperLeft/lowerLeft 對應起點（y=0），upperRight/lowerRight 對應終點（y=20），
        // 兩者的 x 分別落在 5-3=2 與 5+3=8（左右哪一邊由法線方向決定，只驗證數值集合）。
        QCOMPARE(q.upperLeft.y, 0.0);
        QCOMPARE(q.lowerLeft.y, 0.0);
        QCOMPARE(q.upperRight.y, 20.0);
        QCOMPARE(q.lowerRight.y, 20.0);
        QCOMPARE(q.upperLeft.x, q.upperRight.x);   // 同一側的 x 沿著線的方向不變
        QCOMPARE(q.lowerLeft.x, q.lowerRight.x);
        QCOMPARE(q.upperLeft.x - q.lowerLeft.x != 0.0, true);  // 兩側確實分開了
        const double width = std::abs(q.upperLeft.x - q.lowerLeft.x);
        QCOMPARE(width, 6.0);  // 半寬 3 的兩倍
        QVERIFY((qFuzzyCompare(q.upperLeft.x, 2.0) && qFuzzyCompare(q.lowerLeft.x, 8.0)) ||
               (qFuzzyCompare(q.upperLeft.x, 8.0) && qFuzzyCompare(q.lowerLeft.x, 2.0)));
    }

    void diagonalStrokeKeepsRibbonWidthConstant() {
        // 45 度斜線：驗算法線方向必須是 (-1,1)/sqrt(2)，不是隨便選一個垂直向量。
        const std::vector<PointF> points = {{0.0, 0.0}, {10.0, 10.0}};
        const auto quads = ribbonQuadsFromStroke(points, std::sqrt(2.0));
        QCOMPARE(quads.size(), std::size_t{1});
        const QuadPoint& q = quads.front();
        // 半寬 sqrt(2)，法線 (-1,1)/sqrt(2) * sqrt(2) = (-1, 1)。
        QVERIFY(qFuzzyCompare(q.upperLeft.x, -1.0));
        QVERIFY(qFuzzyCompare(q.upperLeft.y, 1.0));
        QVERIFY(qFuzzyCompare(q.lowerLeft.x, 1.0));
        QVERIFY(qFuzzyCompare(q.lowerLeft.y, -1.0));
        QVERIFY(qFuzzyCompare(q.upperRight.x, 9.0));
        QVERIFY(qFuzzyCompare(q.upperRight.y, 11.0));
    }

    void multiSegmentStrokeProducesOneQuadPerSegment() {
        const std::vector<PointF> points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        const auto quads = ribbonQuadsFromStroke(points, 1.0);
        QCOMPARE(quads.size(), std::size_t{3});
    }

    void degenerateSegmentsAreSkippedNotZeroSized() {
        // 重複點（使用者手抖停在原地）：那一段沒有方向，必須整段跳過，
        // 不能產生寬度為零的退化 quad——那種 quad 在渲染時完全不可見，
        // 卻仍然佔掉一組 QuadPoints，會讓 /Rect 的外接框莫名其妙地不變寬。
        const std::vector<PointF> points = {{0, 0}, {0, 0}, {5, 0}};
        const auto quads = ribbonQuadsFromStroke(points, 1.0);
        QCOMPARE(quads.size(), std::size_t{1});
    }

    void fewerThanTwoPointsProducesNoQuads() {
        QVERIFY(ribbonQuadsFromStroke({}, 1.0).empty());
        QVERIFY(ribbonQuadsFromStroke({{0, 0}}, 1.0).empty());
    }

    void nonPositiveHalfWidthProducesNoQuads() {
        const std::vector<PointF> points = {{0, 0}, {10, 0}};
        QVERIFY(ribbonQuadsFromStroke(points, 0.0).empty());
        QVERIFY(ribbonQuadsFromStroke(points, -1.0).empty());
    }
};

QTEST_APPLESS_MAIN(TestAnnfamilyDomain)
#include "test_annfamily_domain.moc"
