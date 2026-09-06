// app/annotation_family_builders.h 測試（WP24）。
//
// 這一層是「使用者操作 → domain::Annotation」的轉換，沒有事件迴圈也沒有檔案
// I/O，因此每個案例都可以用手算的期望值精確核對——尤其是 Callout 引線起點的
// 自動吸附座標與 Fit Box 換行後的框高，這兩項是任務說明裡點名要驗的。
//
// 逐欄位比對而不是整個結構丟給 QCOMPARE：這個專案沒有為 domain 的幾何型別
// 註冊 QTest::toString，逐欄位比對才會在失敗時印出「哪一個座標不對」，
// 而不是一段無法閱讀的位元組。

#include <QtTest>

#include "app/annotation_family_builders.h"

using namespace alioth;
using namespace alioth::domain;

namespace {
void expectPoint(const PointF& actual, double x, double y) {
    QCOMPARE(actual.x, x);
    QCOMPARE(actual.y, y);
}
}  // namespace

class TestAnnfamilyBuilders : public QObject {
    Q_OBJECT

private slots:
    void highlightAreaProducesOneQuadFromTheDraggedRect() {
        app::HighlightAreaRequest request;
        request.pageRect = RectF{10, 20, 110, 40};
        request.color = ColorRgb{1.0, 0.9, 0.1};
        request.opacity = 0.35;

        const Annotation annotation = app::buildHighlightArea(request);
        QCOMPARE(annotation.type(), AnnotationType::Highlight);
        QCOMPARE(annotation.opacity, 0.35);
        const auto* markup = std::get_if<TextMarkupGeometry>(&annotation.geometry);
        QVERIFY(markup != nullptr);
        QCOMPARE(markup->quads.size(), std::size_t{1});
        const QuadPoint& q = markup->quads.front();
        expectPoint(q.upperLeft, 10, 40);
        expectPoint(q.upperRight, 110, 40);
        expectPoint(q.lowerLeft, 10, 20);
        expectPoint(q.lowerRight, 110, 20);
    }

    void freeHighlightMatchesTheDomainRibbonFunctionExactly() {
        app::FreeHighlightRequest request;
        request.strokePoints = {{0, 0}, {10, 0}, {10, 10}};
        request.halfWidth = 2.0;

        const app::FreeHighlightBuildResult result = app::buildFreeHighlight(request);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        const auto* markup = std::get_if<TextMarkupGeometry>(&result.annotation.geometry);
        QVERIFY(markup != nullptr);
        const auto expected = ribbonQuadsFromStroke(request.strokePoints, request.halfWidth);
        QCOMPARE(markup->quads.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expectPoint(markup->quads[i].upperLeft, expected[i].upperLeft.x, expected[i].upperLeft.y);
            expectPoint(markup->quads[i].lowerRight, expected[i].lowerRight.x,
                       expected[i].lowerRight.y);
        }
    }

    void freeHighlightRejectsDegenerateStrokes() {
        app::FreeHighlightRequest single;
        single.strokePoints = {{0, 0}};
        QVERIFY(!app::buildFreeHighlight(single).ok);

        app::FreeHighlightRequest zeroWidth;
        zeroWidth.strokePoints = {{0, 0}, {10, 0}};
        zeroWidth.halfWidth = 0.0;
        QVERIFY(!app::buildFreeHighlight(zeroWidth).ok);
    }

    void caretBuilderCopiesFieldsVerbatim() {
        app::CaretRequest request;
        request.rect = RectF{1, 2, 3, 4};
        request.symbol = CaretSymbol::Paragraph;
        request.color = ColorRgb{0.1, 0.2, 0.3};

        const Annotation annotation = app::buildCaret(request);
        QCOMPARE(annotation.type(), AnnotationType::Caret);
        QCOMPARE(annotation.rect.left, request.rect.left);
        QCOMPARE(annotation.rect.top, request.rect.top);
        QCOMPARE(annotation.color.r, request.color.r);
        QCOMPARE(annotation.color.g, request.color.g);
        QCOMPARE(annotation.color.b, request.color.b);
        const auto* caret = std::get_if<CaretGeometry>(&annotation.geometry);
        QVERIFY(caret != nullptr);
        QCOMPARE(caret->symbol, CaretSymbol::Paragraph);
    }

    void typewriterForcesZeroBorderWidthEvenIfRequested() {
        app::FreeTextRequest request;
        request.rect = RectF{0, 0, 200, 40};
        request.text = "Hi";
        request.borderWidth = 3.0;  // 刻意給非零邊框寬度
        request.autoFit = false;

        const app::FreeTextBuildResult textBox = app::buildTextBox(request);
        QVERIFY2(textBox.ok, textBox.diagnostic.c_str());
        QCOMPARE(textBox.annotation.border.width, 3.0);

        const app::FreeTextBuildResult typewriter = app::buildTypewriter(request);
        QVERIFY2(typewriter.ok, typewriter.diagnostic.c_str());
        QCOMPARE(typewriter.annotation.border.width, 0.0);
        const auto* geometry = std::get_if<FreeTextGeometry>(&typewriter.annotation.geometry);
        QVERIFY(geometry != nullptr);
        QCOMPARE(geometry->intent, FreeTextIntent::Typewriter);
    }

    void autoFitRecomputesHeightFromWrappedLineCount() {
        // 與 test_annfamily_text_layout.cpp 的
        // fitBoxHeightMatchesHandComputedLineCount 同一組手算數字：
        // 7 個 "AAAA"（字級 10）在寬度 100（可用寬度 96）下換成 3/3/1 三行，
        // 框高應為 3 * (10*1.2) + 2*2 = 40。
        app::FreeTextRequest request;
        request.rect = RectF{0, 500, 100, 600};
        request.text = "AAAA AAAA AAAA AAAA AAAA AAAA AAAA";
        request.fontSize = 10.0;
        request.autoFit = true;

        const app::FreeTextBuildResult result = app::buildTextBox(request);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(qFuzzyCompare(result.annotation.rect.top, 600.0));
        QVERIFY(qFuzzyCompare(result.annotation.rect.left, 0.0));
        QVERIFY(qFuzzyCompare(result.annotation.rect.right, 100.0));
        QVERIFY(qAbs(result.annotation.rect.bottom - 560.0) < 1e-6);
        QVERIFY(qAbs(result.annotation.rect.height() - 40.0) < 1e-6);
    }

    void nearestRectBoundaryPointClampsToTheClosestEdge() {
        const RectF rect{0, 0, 100, 50};

        // 正右方、y 在範圍內：貼到右緣，y 不變。
        expectPoint(app::nearestRectBoundaryPoint(rect, PointF{150, 25}), 100, 25);
        // 正下方、x 在範圍內：貼到下緣，x 不變。
        expectPoint(app::nearestRectBoundaryPoint(rect, PointF{50, -30}), 50, 0);
        // 對角線外側：兩軸都夾住，落在角上。
        expectPoint(app::nearestRectBoundaryPoint(rect, PointF{200, 200}), 100, 50);
        // 落在框內、離下緣最近（distBottom=20 < distLeft=30、distRight=70、distTop=30）：
        // 貼到下緣，x 不變。
        expectPoint(app::nearestRectBoundaryPoint(rect, PointF{30, 20}), 30, 0);
    }

    void calloutWiresTheLeaderLineUsingTheNearestBoundaryPoint() {
        app::CalloutRequest request;
        request.rect = RectF{0, 0, 100, 50};
        request.target = PointF{150, 25};  // 正右方，見上一條測試
        request.text = "See here";
        request.autoFit = false;
        request.ending = LineEnding::ClosedArrow;

        const app::FreeTextBuildResult result = app::buildCallout(request);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        const auto* geometry = std::get_if<FreeTextGeometry>(&result.annotation.geometry);
        QVERIFY(geometry != nullptr);
        QVERIFY(geometry->intent == FreeTextIntent::Callout);
        QVERIFY(geometry->callout.has_value());
        expectPoint(geometry->callout->start, 100, 25);
        expectPoint(geometry->callout->end, request.target.x, request.target.y);
        QVERIFY(!geometry->callout->knee.has_value());
        QCOMPARE(geometry->callout->ending, LineEnding::ClosedArrow);
    }

    void calloutWithKneeAnchorsToTheKneeNotTheFarTarget() {
        app::CalloutRequest request;
        request.rect = RectF{0, 0, 100, 50};
        request.knee = PointF{150, 25};   // 折點在正右方
        request.target = PointF{500, 25};  // 最終目標在更遠處
        request.text = "x";
        request.autoFit = false;

        const app::FreeTextBuildResult result = app::buildCallout(request);
        QVERIFY(result.ok);
        const auto* geometry = std::get_if<FreeTextGeometry>(&result.annotation.geometry);
        QVERIFY(geometry != nullptr && geometry->callout.has_value());
        // 起點要吸附到「離折點最近的框緣」，不是離最終目標最近的框緣——
        // 折點在正右方，因此仍是右緣中點。
        expectPoint(geometry->callout->start, 100, 25);
        QVERIFY(geometry->callout->knee.has_value());
        expectPoint(geometry->callout->knee.value(), request.knee->x, request.knee->y);
        expectPoint(geometry->callout->end, request.target.x, request.target.y);
    }
};

QTEST_APPLESS_MAIN(TestAnnfamilyBuilders)
#include "test_annfamily_builders.moc"
