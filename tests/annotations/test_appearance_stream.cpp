// 外觀串流產生器測試（WBS 4.2 / 4.3）。
//
// PRD 把外觀串流列為全案最高風險節點，理由是它的錯誤不會當機也不會報錯，
// 只會在某些檢視器上「看起來怪怪的」。因此這裡測的不是「有沒有輸出」，
// 而是輸出的每一個數字。

#include <QtTest>

#include <cmath>
#include <limits>

#include "content_stream_check.h"
#include "domain/annotation.h"
#include "engine/annotations/appearance_stream.h"

using namespace alioth::domain;
using namespace alioth::engine::annotations;
using alioth::test::checkContentStream;
using alioth::test::ContentStreamReport;

// QTest 的資料驅動欄位走 QMetaType，自訂型別必須先註冊才能當成欄位型別。
Q_DECLARE_METATYPE(alioth::domain::RectF)

namespace {

QByteArray bytes(const std::string& s) { return QByteArray::fromStdString(s); }

Annotation makeHighlight(std::vector<QuadPoint> quads) {
    Annotation annotation{};
    annotation.color = ColorRgb{1.0, 1.0, 0.0};
    annotation.opacity = 0.4;
    annotation.geometry = TextMarkupGeometry{TextMarkupKind::Highlight, std::move(quads)};
    return annotation;
}

Annotation makeSquare(const RectF& rect) {
    Annotation annotation{};
    annotation.rect = rect;
    annotation.color = ColorRgb{1.0, 0.0, 0.0};
    annotation.border.width = 2.0;
    annotation.geometry = ShapeGeometry{ShapeKind::Square};
    return annotation;
}

// 一行文字的 quad，軸對齊。
QuadPoint lineQuad(double left, double bottom, double right, double top) {
    return quadFromPageRect(RectF{left, bottom, right, top});
}

}  // namespace

class TestAppearanceStream : public QObject {
    Q_OBJECT

private slots:
    void numbersAreFormattedWithoutLocaleOrTrailingZeros() {
        QCOMPARE(QString::fromStdString(formatNumber(0.0)), QStringLiteral("0"));
        QCOMPARE(QString::fromStdString(formatNumber(1.0)), QStringLiteral("1"));
        QCOMPARE(QString::fromStdString(formatNumber(1.5)), QStringLiteral("1.5"));
        QCOMPARE(QString::fromStdString(formatNumber(-2.25)), QStringLiteral("-2.25"));
        QCOMPARE(QString::fromStdString(formatNumber(100.0 / 3.0)), QStringLiteral("33.3333"));
        // 四捨五入到零時不得留下 "-0"，那在部分剖析器上是合法但礙眼的輸出。
        QCOMPARE(QString::fromStdString(formatNumber(-0.00001)), QStringLiteral("0"));
        // 非有限值不能讓串流變成 "nan" 或 "inf"，那會直接讓整份 PDF 無法剖析。
        QCOMPARE(QString::fromStdString(formatNumber(std::nan(""))), QStringLiteral("0"));
        QCOMPARE(QString::fromStdString(formatNumber(std::numeric_limits<double>::infinity())),
                 QStringLiteral("0"));
        // 有限但荒謬的值夾在 PDF 實數的實用範圍內，避免整數轉換溢位。
        QCOMPARE(QString::fromStdString(formatNumber(1.0e30)), QStringLiteral("1000000000"));
    }

    // 每一種註解型別的串流都必須可分詞、運算子已知、運算元數量正確、q/Q 配對。
    void everyAnnotationTypeProducesWellFormedContent_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("highlight") << 0;
        QTest::newRow("underline") << 1;
        QTest::newRow("strikeout") << 2;
        QTest::newRow("squiggly") << 3;
        QTest::newRow("square") << 4;
        QTest::newRow("circle") << 5;
        QTest::newRow("line") << 6;
        QTest::newRow("ink") << 7;
        QTest::newRow("text-note") << 8;
    }

    void everyAnnotationTypeProducesWellFormedContent() {
        QFETCH(int, kind);

        Annotation annotation{};
        annotation.color = ColorRgb{0.1, 0.2, 0.3};
        annotation.opacity = 0.75;
        annotation.border.width = 1.5;
        annotation.rect = RectF{10.0, 20.0, 110.0, 80.0};

        const std::vector<QuadPoint> quads{lineQuad(10.0, 60.0, 110.0, 80.0),
                                           lineQuad(10.0, 20.0, 90.0, 40.0)};
        switch (kind) {
            case 0: annotation.geometry = TextMarkupGeometry{TextMarkupKind::Highlight, quads}; break;
            case 1: annotation.geometry = TextMarkupGeometry{TextMarkupKind::Underline, quads}; break;
            case 2: annotation.geometry = TextMarkupGeometry{TextMarkupKind::StrikeOut, quads}; break;
            case 3: annotation.geometry = TextMarkupGeometry{TextMarkupKind::Squiggly, quads}; break;
            case 4: annotation.geometry = ShapeGeometry{ShapeKind::Square}; break;
            case 5: annotation.geometry = ShapeGeometry{ShapeKind::Circle}; break;
            case 6:
                annotation.geometry =
                    LineGeometry{PointF{10.0, 20.0}, PointF{110.0, 80.0}, LineEnding::OpenArrow,
                                 LineEnding::ClosedArrow};
                break;
            case 7:
                annotation.geometry = InkGeometry{{{PointF{10.0, 20.0}, PointF{50.0, 60.0},
                                                    PointF{80.0, 30.0}},
                                                   {PointF{100.0, 70.0}}}};
                break;
            default: annotation.geometry = TextNoteGeometry{TextNoteIcon::Note, false}; break;
        }
        if (kind == 4 || kind == 5) annotation.interiorColor = ColorRgb{0.9, 0.9, 0.2};

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());

        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.maxSaveDepth, 1);
        QCOMPARE(report.operators.first(), QStringLiteral("q"));
        QCOMPARE(report.operators.last(), QStringLiteral("Q"));

        QVERIFY(!appearance.bbox.isEmpty());
    }

    // 多行螢光筆：每一組 QuadPoints 各自成一個子路徑，最後只塗一次。
    void highlightProducesOneSubpathPerQuad() {
        const std::vector<QuadPoint> quads{lineQuad(72.0, 700.0, 300.0, 712.0),
                                           lineQuad(72.0, 686.0, 280.0, 698.0),
                                           lineQuad(72.0, 672.0, 150.0, 684.0)};
        const Appearance appearance = generateAppearance(makeHighlight(quads));
        QVERIFY(appearance.valid);

        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("m")), 3);
        QCOMPARE(report.countOperator(QStringLiteral("l")), 9);
        QCOMPARE(report.countOperator(QStringLiteral("h")), 3);
        // 三個子路徑共用一次填色：拆成三次 f 在 Multiply 混合下會在重疊處疊色。
        QCOMPARE(report.countOperator(QStringLiteral("f")), 1);
        QCOMPARE(report.countOperator(QStringLiteral("S")), 0);

        QCOMPARE(appearance.bbox.left, 72.0);
        QCOMPARE(appearance.bbox.bottom, 672.0);
        QCOMPARE(appearance.bbox.right, 300.0);
        QCOMPARE(appearance.bbox.top, 712.0);
    }

    // 角序寫反是 QuadPoints 最常見的缺陷，逐一比對座標序列。
    void highlightPathFollowsQuadCornerOrder() {
        const Appearance appearance =
            generateAppearance(makeHighlight({lineQuad(10.0, 20.0, 60.0, 32.0)}));
        QVERIFY(appearance.valid);

        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));

        // 前三個數字是填色 rg 的成分，之後才是路徑座標。
        const QList<double> expected{10.0, 32.0,   // 左上
                                     60.0, 32.0,   // 右上
                                     60.0, 20.0,   // 右下
                                     10.0, 20.0};  // 左下
        const QList<double> path = report.numbers.mid(report.numbers.size() - expected.size());
        QCOMPARE(path, expected);
    }

    // 旋轉頁：QuadPoints 必須落在未旋轉的頁面座標系上（PRD-ANN-001）。
    void quadPointsAreCorrectOnRotatedPages_data() {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<RectF>("expectedPageRect");
        // 頁面 200×400；裝置矩形取自畫面上同一塊區域。
        QTest::newRow("0deg") << 0 << RectF{20.0, 350.0, 60.0, 370.0};
        QTest::newRow("90deg") << 1 << RectF{30.0, 20.0, 50.0, 60.0};
        QTest::newRow("180deg") << 2 << RectF{140.0, 30.0, 180.0, 50.0};
        QTest::newRow("270deg") << 3 << RectF{150.0, 340.0, 170.0, 380.0};
    }

    void quadPointsAreCorrectOnRotatedPages() {
        QFETCH(int, rotation);
        QFETCH(RectF, expectedPageRect);

        const PageTransform transform(SizeF{200.0, 400.0}, 1.0,
                                      static_cast<Rotation>(rotation));
        // 未旋轉時，裝置 (20,30)-(60,50) 對應頁面 (20,350)-(60,370)。
        const RectF deviceRect{20.0, 30.0, 60.0, 50.0};
        const QuadPoint quad = quadFromDeviceRect(transform, deviceRect);

        QCOMPARE(quad.lowerLeft.x, expectedPageRect.left);
        QCOMPARE(quad.lowerLeft.y, expectedPageRect.bottom);
        QCOMPARE(quad.upperRight.x, expectedPageRect.right);
        QCOMPARE(quad.upperRight.y, expectedPageRect.top);
        // 角序必須始終一致：上緣的 Y 大於下緣，與頁面 Y 向上一致。
        QVERIFY(quad.upperLeft.y > quad.lowerLeft.y);
        QVERIFY(quad.upperRight.x > quad.upperLeft.x);

        const Appearance appearance = generateAppearance(makeHighlight({quad}));
        QVERIFY(appearance.valid);
        QCOMPARE(appearance.bbox, expectedPageRect);
    }

    // 底線沿 quad 自己的方向走，位置與粗細由文字高度決定。
    void underlineSitsNearTheBaselineWithProportionalThickness() {
        Annotation annotation{};
        annotation.color = ColorRgb{0.0, 0.0, 1.0};
        annotation.geometry =
            TextMarkupGeometry{TextMarkupKind::Underline, {lineQuad(10.0, 100.0, 110.0, 132.0)}};

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);

        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("S")), 1);
        QCOMPARE(report.countOperator(QStringLiteral("f")), 0);

        const double height = 32.0;
        const double expectedThickness = height / 16.0;
        const double expectedY = 100.0 + height / 16.0;
        // 線寬 → 描邊色 → 起點 → 終點。
        QCOMPARE(report.numbers.at(3), expectedThickness);
        QCOMPARE(report.numbers.at(4), 10.0);
        QCOMPARE(report.numbers.at(5), expectedY);
        QCOMPARE(report.numbers.at(6), 110.0);
        QCOMPARE(report.numbers.at(7), expectedY);
        // BBox 要含進半條線寬，否則高倍率下線會被 BBox 裁掉一半。
        QCOMPARE(appearance.bbox.bottom, expectedY - expectedThickness / 2.0);
    }

    void strikeOutRunsThroughTheMiddleOfTheQuad() {
        Annotation annotation{};
        annotation.geometry =
            TextMarkupGeometry{TextMarkupKind::StrikeOut, {lineQuad(10.0, 100.0, 110.0, 132.0)}};

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.numbers.at(5), 116.0);  // 100 + 32 * 0.5
    }

    // 透明度必須落在 /ExtGState，而不是被偷偷做成淡色。
    void opacityGoesIntoExtGState() {
        const Appearance appearance =
            generateAppearance(makeHighlight({lineQuad(0.0, 0.0, 10.0, 10.0)}));
        QVERIFY(appearance.valid);
        QVERIFY(appearance.requiresResources());
        QCOMPARE(appearance.extGStates.size(), std::size_t{1});
        QCOMPARE(appearance.extGStates.front().fillAlpha, 0.4);
        QCOMPARE(appearance.extGStates.front().strokeAlpha, 0.4);
        // 螢光筆一定是 Multiply，否則會蓋掉底下的文字。
        QVERIFY(appearance.extGStates.front().blend == BlendMode::Multiply);
        QVERIFY(!appearance.resourcesElided);

        QVERIFY(appearance.content.find("/GS0 gs") != std::string::npos);
        const QString resources = QString::fromStdString(appearance.resourcesDictionary());
        QVERIFY2(resources.contains(QStringLiteral("/CA 0.4")), qPrintable(resources));
        QVERIFY2(resources.contains(QStringLiteral("/ca 0.4")), qPrintable(resources));
        QVERIFY2(resources.contains(QStringLiteral("/BM /Multiply")), qPrintable(resources));
        QVERIFY2(resources.contains(QStringLiteral("/ExtGState")), qPrintable(resources));
    }

    // 不透明且非螢光筆的註解不該憑空生出一個 /ExtGState。
    void opaqueShapeNeedsNoExtGState() {
        Annotation annotation = makeSquare(RectF{0.0, 0.0, 50.0, 50.0});
        annotation.opacity = 1.0;
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        QVERIFY(!appearance.requiresResources());
        QCOMPARE(QString::fromStdString(appearance.resourcesDictionary()), QStringLiteral("<< >>"));
        QVERIFY(appearance.content.find("gs") == std::string::npos);
    }

    // 目標容器無法附帶 /Resources 時，必須改為不輸出 gs 並明確標記降級，
    // 而不是留下一個引用不存在資源的串流。
    void elidesGraphicsStateWhenResourcesUnavailable() {
        AppearanceOptions options{};
        options.resourcesSupported = false;
        const Appearance appearance =
            generateAppearance(makeHighlight({lineQuad(0.0, 0.0, 10.0, 10.0)}), options);
        QVERIFY(appearance.valid);
        QVERIFY(appearance.resourcesElided);
        QVERIFY(appearance.extGStates.empty());
        QVERIFY(appearance.content.find("gs") == std::string::npos);

        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
    }

    // 描邊往內縮半個線寬，讓筆跡完整落在 /Rect 內。
    void squareStrokeIsInsetByHalfTheBorderWidth() {
        const Appearance appearance = generateAppearance(makeSquare(RectF{10.0, 10.0, 110.0, 60.0}));
        QVERIFY(appearance.valid);
        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("re")), 1);

        const QList<double> re = report.numbers.mid(report.numbers.size() - 4);
        QCOMPARE(re, (QList<double>{11.0, 11.0, 98.0, 48.0}));
        QCOMPARE(appearance.bbox, (RectF{10.0, 10.0, 110.0, 60.0}));
        // 只有描邊沒有填色時用 S。
        QCOMPARE(report.countOperator(QStringLiteral("S")), 1);
    }

    void filledSquareUsesFillAndStrokeOperator() {
        Annotation annotation = makeSquare(RectF{0.0, 0.0, 20.0, 20.0});
        annotation.interiorColor = ColorRgb{0.0, 1.0, 0.0};
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("B")), 1);
    }

    void circleIsFourBezierSegments() {
        Annotation annotation = makeSquare(RectF{0.0, 0.0, 100.0, 50.0});
        annotation.geometry = ShapeGeometry{ShapeKind::Circle};
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("c")), 4);
        QCOMPARE(report.countOperator(QStringLiteral("m")), 1);
        QCOMPARE(report.countOperator(QStringLiteral("h")), 1);
    }

    // 手繪：每條筆畫一個子路徑，BBox 要含進半個筆寬的圓頭。
    void inkProducesOneSubpathPerStrokeAndInflatesBounds() {
        Annotation annotation{};
        annotation.border.width = 4.0;
        annotation.geometry = InkGeometry{{{PointF{10.0, 10.0}, PointF{20.0, 30.0}},
                                           {PointF{40.0, 50.0}, PointF{60.0, 50.0},
                                            PointF{60.0, 20.0}}}};
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("m")), 2);
        QCOMPARE(report.countOperator(QStringLiteral("l")), 3);
        QCOMPARE(report.countOperator(QStringLiteral("S")), 1);
        QCOMPARE(appearance.bbox, (RectF{8.0, 8.0, 62.0, 52.0}));
    }

    void singlePointInkStrokeStillDrawsADot() {
        Annotation annotation{};
        annotation.border.width = 3.0;
        annotation.geometry = InkGeometry{{{PointF{25.0, 25.0}}}};
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        const ContentStreamReport report = checkContentStream(bytes(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("m")), 1);
        QCOMPARE(report.countOperator(QStringLiteral("l")), 1);
        QVERIFY(!appearance.bbox.isEmpty());
    }

    // 便利貼圖示在旋轉頁上要靠 /Matrix 反轉回正，且 BBox 必須是正方形，
    // 否則 §12.5.5 的外觀對映會把圖示拉扁。
    void textNoteCounterRotatesOnRotatedPages() {
        Annotation annotation{};
        annotation.rect = RectF{50.0, 300.0, 70.0, 320.0};
        annotation.geometry = TextNoteGeometry{TextNoteIcon::Note, false};

        const Appearance upright = generateAppearance(annotation);
        QVERIFY(upright.valid);
        QCOMPARE(upright.matrix, (std::array<double, 6>{1, 0, 0, 1, 0, 0}));
        QCOMPARE(upright.bbox.width(), upright.bbox.height());

        AppearanceOptions rotated{};
        rotated.pageRotation = Rotation::Cw90;
        const Appearance turned = generateAppearance(annotation, rotated);
        QVERIFY(turned.valid);
        // 頁面順時針 90 度 → 外觀逆向轉 90 度，矩陣為 [0 -1 1 0 e f]。
        QCOMPARE(turned.matrix[0], 0.0);
        QCOMPARE(turned.matrix[1], -1.0);
        QCOMPARE(turned.matrix[2], 1.0);
        QCOMPARE(turned.matrix[3], 0.0);
        // 繞 BBox 中心旋轉：中心必須是不動點。
        const double cx = (turned.bbox.left + turned.bbox.right) * 0.5;
        const double cy = (turned.bbox.bottom + turned.bbox.top) * 0.5;
        const double mappedX = turned.matrix[0] * cx + turned.matrix[2] * cy + turned.matrix[4];
        const double mappedY = turned.matrix[1] * cx + turned.matrix[3] * cy + turned.matrix[5];
        QCOMPARE(mappedX, cx);
        QCOMPARE(mappedY, cy);
    }

    // 失敗必須明確：不得產生一份空的但「有效」的外觀串流（IL-4）。
    void invalidInputFailsLoudly_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("markup-without-quads") << 0;
        QTest::newRow("zero-height-quad") << 1;
        QTest::newRow("empty-shape-rect") << 2;
        QTest::newRow("shape-without-stroke-or-fill") << 3;
        QTest::newRow("degenerate-line") << 4;
        QTest::newRow("ink-without-strokes") << 5;
    }

    void invalidInputFailsLoudly() {
        QFETCH(int, kind);
        Annotation annotation{};
        annotation.border.width = 1.0;
        annotation.rect = RectF{0.0, 0.0, 10.0, 10.0};
        switch (kind) {
            case 0: annotation.geometry = TextMarkupGeometry{TextMarkupKind::Highlight, {}}; break;
            case 1:
                annotation.geometry = TextMarkupGeometry{TextMarkupKind::Highlight,
                                                         {lineQuad(0.0, 5.0, 10.0, 5.0)}};
                break;
            case 2:
                annotation.rect = RectF{};
                annotation.geometry = ShapeGeometry{ShapeKind::Square};
                break;
            case 3:
                annotation.border.width = 0.0;
                annotation.geometry = ShapeGeometry{ShapeKind::Circle};
                break;
            case 4:
                annotation.geometry = LineGeometry{PointF{5.0, 5.0}, PointF{5.0, 5.0}};
                break;
            default: annotation.geometry = InkGeometry{}; break;
        }

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(!appearance.valid);
        QVERIFY(!appearance.diagnostic.empty());
        QVERIFY(appearance.content.empty());
    }

    // 純函數：同輸入必得同輸出，不得夾帶時間戳或亂數。
    void generationIsDeterministic() {
        const Annotation annotation = makeHighlight({lineQuad(1.0, 2.0, 3.0, 4.0)});
        const Appearance a = generateAppearance(annotation);
        const Appearance b = generateAppearance(annotation);
        QCOMPARE(QString::fromStdString(a.content), QString::fromStdString(b.content));
        QCOMPARE(QString::fromStdString(a.resourcesDictionary()),
                 QString::fromStdString(b.resourcesDictionary()));
    }
};

QTEST_APPLESS_MAIN(TestAppearanceStream)
#include "test_appearance_stream.moc"
