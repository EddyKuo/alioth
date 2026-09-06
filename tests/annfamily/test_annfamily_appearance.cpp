// engine/annotations/appearance_stream.cpp 對 FreeText 家族與 Caret 的測試
// （WP24：PRD-ANN-005/019/021/022）。
//
// 用 tests/annotations/content_stream_check.h 的語法檢查器逐運算子驗證，
// 而不是只看「有沒有丟例外」——外觀串流的錯誤幾乎都是安靜的（q/Q 沒配對、
// 運算元數量不對），這正是這支測試要攔住的那一類。

#include <QtTest>

#include "annotations/content_stream_check.h"
#include "engine/annotations/appearance_stream.h"
#include "engine/fonts/cjk_font_library.h"

using namespace alioth;
using namespace alioth::domain;
using namespace alioth::engine::annotations;
using alioth::test::checkContentStream;

namespace {

Annotation makeTextBox(const std::string& text, bool withFill) {
    Annotation annotation;
    annotation.rect = RectF{50, 50, 250, 130};
    annotation.color = ColorRgb{0.0, 0.0, 0.0};
    annotation.border.width = 1.0;
    if (withFill) annotation.interiorColor = ColorRgb{1.0, 1.0, 0.8};

    FreeTextGeometry geometry;
    geometry.text = text;
    geometry.fontSize = 12.0;
    geometry.textColor = ColorRgb{0.0, 0.0, 0.0};
    geometry.intent = FreeTextIntent::TextBox;
    annotation.geometry = geometry;
    return annotation;
}

Annotation makeTypewriter(const std::string& text) {
    Annotation annotation = makeTextBox(text, false);
    // Typewriter 的定義就是無邊框：即使呼叫端仍設了邊框寬度，外觀產生器
    // 也必須無視它——這裡刻意保留非零 border.width，驗證的正是這個無視行為。
    auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
    geometry.intent = FreeTextIntent::Typewriter;
    return annotation;
}


Annotation makePolygon(bool cloudy, bool clockwise = false) {
    Annotation annotation;
    annotation.color = ColorRgb{0.85, 0.1, 0.1};
    annotation.border.width = 2.0;
    PolygonGeometry polygon;
    // 一個 100×100 的正方形。逆時針（預設）與順時針各測一次，
    // 因為雲弧的凸向是由頂點順序推出來的。
    if (clockwise) {
        polygon.vertices = {PointF{100, 100}, PointF{100, 200}, PointF{200, 200},
                            PointF{200, 100}};
    } else {
        polygon.vertices = {PointF{100, 100}, PointF{200, 100}, PointF{200, 200},
                            PointF{100, 200}};
    }
    if (cloudy) {
        polygon.borderEffect.effect = BorderEffect::Cloudy;
        polygon.borderEffect.intensity = 1.0;
    }
    annotation.geometry = std::move(polygon);
    annotation.rect = RectF{90, 90, 210, 210};
    return annotation;
}

}  // namespace

class TestAnnfamilyAppearance : public QObject {
    Q_OBJECT

private slots:
    // 雲線（PRD-ANN-002）。一般多邊形畫直邊，雲線畫貝茲弧。
    void cloudyBorderDrawsCurvesWhereAPlainPolygonDrawsLines() {
        const Appearance plain = generateAppearance(makePolygon(false));
        QVERIFY2(plain.valid, plain.diagnostic.c_str());
        const auto plainReport = checkContentStream(QByteArray::fromStdString(plain.content));
        QVERIFY2(plainReport.valid, qPrintable(plainReport.error));
        QCOMPARE(plainReport.countOperator("c"), 0);
        QVERIFY(plainReport.countOperator("l") >= 3);

        const Appearance cloudy = generateAppearance(makePolygon(true));
        QVERIFY2(cloudy.valid, cloudy.diagnostic.c_str());
        const auto cloudyReport = checkContentStream(QByteArray::fromStdString(cloudy.content));
        QVERIFY2(cloudyReport.valid, qPrintable(cloudyReport.error));
        // 每一邊至少一個弧，四邊就至少四個；直線段不再出現。
        QVERIFY(cloudyReport.countOperator("c") >= 4);
        QCOMPARE(cloudyReport.countOperator("l"), 0);
    }

    // 雲弧必須凸向多邊形外側。凸錯邊時形狀看起來像被咬過一口——
    // 這是這個效果最容易做錯的地方，而它不會有任何錯誤訊息。
    void cloudArcsBulgeOutwardRegardlessOfVertexOrder() {
        for (const bool clockwise : {false, true}) {
            const Annotation annotation = makePolygon(true, clockwise);
            const Appearance appearance = generateAppearance(annotation);
            QVERIFY2(appearance.valid, appearance.diagnostic.c_str());

            // 外框要比多邊形本身大：弧凸出去的部分被算進去了。
            // 凸向內側的話外框會等於或小於多邊形。
            QVERIFY2(appearance.bbox.left < 100.0,
                     qPrintable(QStringLiteral("clockwise=%1 外框左緣 %2 沒有向外擴張")
                                    .arg(clockwise)
                                    .arg(appearance.bbox.left)));
            QVERIFY(appearance.bbox.right > 200.0);
            QVERIFY(appearance.bbox.bottom < 100.0);
            QVERIFY(appearance.bbox.top > 200.0);
        }
    }

    void cloudIntensityIsClampedToTheSpecRange() {
        // /I 只定義 0/1/2。超出範圍不該產生失敗，也不該原樣使用——
        // 夾住之後仍然畫得出東西才是對使用者有用的行為。
        Annotation annotation = makePolygon(true);
        auto& polygon = std::get<PolygonGeometry>(annotation.geometry);
        polygon.borderEffect.intensity = 99.0;
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QVERIFY(report.countOperator("c") >= 4);
    }

    void zeroIntensityFallsBackToAPlainPolygon() {
        // /I 為 0 等於沒有效果。畫成雲反而是錯的。
        Annotation annotation = makePolygon(true);
        auto& polygon = std::get<PolygonGeometry>(annotation.geometry);
        polygon.borderEffect.intensity = 0.0;
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY(report.valid);
        QCOMPARE(report.countOperator("c"), 0);
    }

    void textBoxWithBorderProducesChromeAndClipAndText() {
        const Annotation annotation = makeTextBox("Hello", false);
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        QVERIFY(appearance.needsFont);

        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        // 一個矩形是邊框的 "re"，一個是文字裁切的 "re"：兩者都要出現。
        QCOMPARE(report.countOperator("re"), 2);
        QCOMPARE(report.countOperator("S"), 1);   // 邊框描邊（無填色）
        QCOMPARE(report.countOperator("W"), 1);   // 裁切
        QCOMPARE(report.countOperator("Tj"), 1);  // 單行文字
        QCOMPARE(report.countOperator("BT"), 1);
        QCOMPARE(report.countOperator("ET"), 1);
    }

    void typewriterNeverDrawsChromeEvenWithBorderWidthSet() {
        const Annotation annotation = makeTypewriter("Hi");
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());

        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        // 只剩下裁切矩形那一個 "re"，邊框的那個必須不存在；連帶地不該有 "S"/"B"/"f"
        // 這種對邊框路徑的塗繪動作（裁切走的是 "W"+"n"，不算塗繪）。
        QCOMPARE(report.countOperator("re"), 1);
        QCOMPARE(report.countOperator("S"), 0);
        QCOMPARE(report.countOperator("B"), 0);
    }

    void filledTextBoxUsesBOperator() {
        const Annotation annotation = makeTextBox("Hi", true);
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator("B"), 1);  // 有邊框也有填色 → B（stroke+fill）
    }

    void emptyTextDoesNotRequireFontResource() {
        const Annotation annotation = makeTextBox("", false);
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        QVERIFY(!appearance.needsFont);
        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator("BT"), 0);
    }

    // ADR-007 之後，CJK 走內嵌的思源黑體子集。
    //
    // 這一條要驗的**不是**「成功了」，而是成功時內容串流真的切到了另一個字型。
    // 用 /Helv 畫出中文位元組會產生一份看起來有效、實際上是亂碼的外觀——
    // 那正是最難發現的那種錯：generateAppearance 回報成功，qpdf 檢查也通過，
    // 只有真的打開來看才知道畫的是別的東西。
    void cjkTextSwitchesToTheEmbeddedFont() {
        const Annotation annotation = makeTextBox("\xE4\xB8\xAD\xE6\x96\x87", false);  // "中文"
        const Appearance appearance = generateAppearance(annotation);

        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QVERIFY(!appearance.valid);
            QVERIFY(!appearance.diagnostic.empty());
            return;
        }

        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        QVERIFY2(appearance.needsCjkFont, "沒有回報需要 CJK 字型");
        QCOMPARE(appearance.cjkCodepoints.size(), std::size_t(2));

        // 內容串流必須引用 /CJK 而不是只有 /Helv。
        QVERIFY2(appearance.content.find("/CJK") != std::string::npos,
                 "內容串流沒有切換到 CJK 字型");

        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
    }

    void mixedLatinAndCjkSwitchesFontsPerRun() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        // "A中B"：拉丁與 CJK 的編碼方式不同（單位元組 vs 雙位元組），
        // 整段用同一個字型畫的話，不是中文變亂碼就是拉丁字被當成雙位元組讀掉。
        const Annotation annotation = makeTextBox("A\xE4\xB8\xAD" "B", false);
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        QVERIFY(appearance.needsCjkFont);
        QCOMPARE(appearance.cjkCodepoints.size(), std::size_t(1));
        // 兩種字型都要出現，而且每切換一次就要重下一次 Tf。
        QVERIFY(appearance.content.find("/Helv") != std::string::npos);
        QVERIFY(appearance.content.find("/CJK") != std::string::npos);
        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QVERIFY(report.countOperator("Tf") >= 2);
    }

    void asciiOnlyTextDoesNotDragInTheCjkFont() {
        // 純英文的註解不該把字型子集拖進來。
        const Annotation annotation = makeTextBox("Hello", false);
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(appearance.valid);
        QVERIFY(!appearance.needsCjkFont);
        QVERIFY(appearance.cjkCodepoints.empty());
        QVERIFY(appearance.content.find("/CJK") == std::string::npos);
    }

    void emptyRectFailsExplicitly() {
        Annotation annotation = makeTextBox("x", false);
        annotation.rect = RectF{};
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(!appearance.valid);
    }

    void calloutDrawsLeaderLineAndExpandsBoundsToTheTarget() {
        Annotation annotation = makeTextBox("Note", false);
        annotation.rect = RectF{100, 100, 220, 150};
        auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
        geometry.intent = FreeTextIntent::Callout;
        CalloutLine line;
        line.start = PointF{100, 125};  // 貼在框的左緣中點
        line.end = PointF{20, 300};     // 指向框外、遠處的一點
        line.ending = LineEnding::OpenArrow;
        geometry.callout = line;

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());

        // /BBox 必須把引線終點也包進去，否則 Acrobat 會把箭頭裁掉。
        QVERIFY(appearance.bbox.left <= 20.0);
        QVERIFY(appearance.bbox.top >= 300.0);

        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        // 引線本身一段 "S"、箭頭是開放式（不封閉）也是 "S"：邊框（無填色）另一個 "S"。
        // 至少要有 2 個 "S"（引線 + 箭頭），不檢查上限，因為邊框也會貢獻一個。
        QVERIFY(report.countOperator("S") >= 2);
    }

    // PRD-ANN-031 縮排：可用寬度＝內縮後的框寬－縮排量，換行門檻與
    // engine/annotations/text_layout.h 的 wrappingBreaksExactlyAtTheWordThatWouldOverflow
    // 用的是同一組手算數字（"AAAA BBBB" 合併寬 56.14pt，60pt 內不換行、
    // 40pt 內必須換行），這裡只是換一個角度驗證：indentPt 真的會讓
    // 同一份文字在同一個框裡從一行變兩行，而不是被外觀產生器忽略掉。
    //
    // 刻意不用 checkContentStream：不換行時單一個 Tj 的字串常值裡含空白
    // （"(AAAA BBBB)"），而那個檢查器的分詞器只用空白切詞（見檔頭註解的
    // 已知限制），會把這個合法字串誤判成兩個 token。直接數 "Tj" 出現次數
    // 一樣能驗證換行結果，不會踩到這個限制。
    void indentNarrowsAvailableWidthAndCanForceExtraWrapping() {
        Annotation annotation = makeTextBox("AAAA BBBB", false);
        annotation.rect = RectF{0, 0, 68, 100};  // 內縮 2pt 兩側 → 可用寬度 64pt
        auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
        geometry.fontSize = 10.0;

        const Appearance withoutIndent = generateAppearance(annotation);
        QVERIFY2(withoutIndent.valid, withoutIndent.diagnostic.c_str());
        QCOMPARE(static_cast<int>(QByteArray::fromStdString(withoutIndent.content).count(" Tj")),
                1);  // 64pt 夠寬，不換行

        geometry.indentPt = 30.0;  // 可用寬度剩 34pt，低於兩個詞需要的 40pt 門檻
        const Appearance withIndent = generateAppearance(annotation);
        QVERIFY2(withIndent.valid, withIndent.diagnostic.c_str());
        QCOMPARE(static_cast<int>(QByteArray::fromStdString(withIndent.content).count(" Tj")),
                2);  // 縮排後被迫換成兩行
    }

    void caretNoneSymbolDrawsAStrokedWedgeWithInflatedBounds() {
        Annotation annotation;
        annotation.rect = RectF{0, 0, 20, 20};
        annotation.border.width = 2.0;
        annotation.geometry = CaretGeometry{CaretSymbol::None};

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        QVERIFY(!appearance.needsFont);

        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator("S"), 1);
        QCOMPARE(report.countOperator("f"), 0);

        // 描邊會往外膨脹半個線寬（見 writeCaretInsertMark 的 bounds.inflate）。
        QVERIFY(appearance.bbox.left < 0.0);
        QVERIFY(appearance.bbox.right > 20.0);
    }

    void caretParagraphSymbolFillsTwoShapesWithoutInflatingBounds() {
        Annotation annotation;
        annotation.rect = RectF{0, 0, 20, 20};
        annotation.geometry = CaretGeometry{CaretSymbol::Paragraph};

        const Appearance appearance = generateAppearance(annotation);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());

        const auto report = checkContentStream(QByteArray::fromStdString(appearance.content));
        QVERIFY2(report.valid, qPrintable(report.error));
        // 莖（矩形）與碗（橢圓）各自獨立填色。
        QCOMPARE(report.countOperator("f"), 2);
        QCOMPARE(report.countOperator("re"), 1);
        QCOMPARE(report.countOperator("c"), 4);  // 橢圓的四段貝茲曲線

        // /BBox 就是 /Rect 本身，沒有描邊膨脹。
        QCOMPARE(appearance.bbox.left, 0.0);
        QCOMPARE(appearance.bbox.bottom, 0.0);
        QCOMPARE(appearance.bbox.right, 20.0);
        QCOMPARE(appearance.bbox.top, 20.0);
    }

    void caretRejectsEmptyRect() {
        Annotation annotation;
        annotation.geometry = CaretGeometry{CaretSymbol::None};
        const Appearance appearance = generateAppearance(annotation);
        QVERIFY(!appearance.valid);
    }
};

QTEST_APPLESS_MAIN(TestAnnfamilyAppearance)
#include "test_annfamily_appearance.moc"
