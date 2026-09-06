// 文字層領域模型測試。純 C++，不載入 PDFium——
// 選取幾何的邏輯要能在沒有文件的情況下驗證，這正是領域層存在的理由。

#include <QtTest>

#include <string>
#include <vector>

#include "domain/text_layer.h"

using namespace alioth::domain;

namespace {

// 造兩行等寬字元：第一行 y 100–112，第二行 y 80–92，每個字元寬 10。
PageTextLayer makeLayer() {
    const std::string line0 = "Hello world";
    const std::string line1 = "second";
    std::vector<TextChar> chars;
    std::int32_t index = 0;

    auto append = [&](const std::string& text, double bottom, double top, std::int32_t line) {
        double x = 20.0;
        for (const char c : text) {
            TextChar ch;
            ch.index = index++;
            ch.unicode = static_cast<char32_t>(c);
            ch.box = RectF{x, bottom, x + 10.0, top};
            ch.lineIndex = line;
            chars.push_back(ch);
            x += 10.0;
        }
    };

    append(line0, 100.0, 112.0, 0);
    // 換行字元的外框是退化的，且位置不代表任何可見內容。
    TextChar newline;
    newline.index = index++;
    newline.unicode = U'\n';
    newline.lineIndex = 0;
    chars.push_back(newline);
    append(line1, 80.0, 92.0, 1);

    return PageTextLayer{0, std::move(chars)};
}

}  // namespace

class TestTextLayer : public QObject {
    Q_OBJECT

private slots:
    void rangeNormalisesAndClamps() {
        QCOMPARE(TextRange(9, 3).normalized(), TextRange(3, 9));
        QCOMPARE(TextRange(-5, 100).clamped(20), TextRange(0, 20));
        QCOMPARE(TextRange::fromCount(4, 3), TextRange(4, 7));
        QVERIFY(TextRange(5, 5).isEmpty());
        QVERIFY(TextRange(5, 7).contains(6));
        QVERIFY(!TextRange(5, 7).contains(7));
    }

    void categorisesCharactersForWordSelection() {
        QCOMPARE(categorize(U'a'), CharCategory::Word);
        QCOMPARE(categorize(U'7'), CharCategory::Word);
        QCOMPARE(categorize(U'_'), CharCategory::Word);
        QCOMPARE(categorize(U' '), CharCategory::Whitespace);
        QCOMPARE(categorize(U'\n'), CharCategory::Control);
        QCOMPARE(categorize(U','), CharCategory::Punctuation);
        QCOMPARE(categorize(U'漢'), CharCategory::Ideograph);
    }

    void extractsTextAsUtf8() {
        const PageTextLayer layer = makeLayer();
        QCOMPARE(layer.text(TextRange::fromCount(0, 5)), std::string("Hello"));
        QCOMPARE(layer.text(layer.fullRange()).substr(0, 11), std::string("Hello world"));

        // 代理對必須合併成一個碼點，否則複製出去的字在其他程式裡是兩個問號。
        std::vector<TextChar> chars(2);
        chars[0].index = 0;
        chars[0].unicode = 0xD83D;
        chars[1].index = 1;
        chars[1].unicode = 0xDE00;
        const PageTextLayer emoji{0, std::move(chars)};
        QCOMPARE(emoji.text(emoji.fullRange()), std::string("\xF0\x9F\x98\x80"));
    }

    void quadsSplitPerLineAndSkipControlChars() {
        const PageTextLayer layer = makeLayer();

        const std::vector<QuadPoint> single = layer.quads(TextRange::fromCount(0, 5));
        QCOMPARE(single.size(), std::size_t(1));
        QCOMPARE(single[0].boundingBox(), RectF(20.0, 100.0, 70.0, 112.0));

        const std::vector<QuadPoint> crossing = layer.quads(layer.fullRange());
        QCOMPARE(crossing.size(), std::size_t(2));
        QCOMPARE(crossing[0].boundingBox(), RectF(20.0, 100.0, 130.0, 112.0));
        QCOMPARE(crossing[1].boundingBox(), RectF(20.0, 80.0, 80.0, 92.0));
    }

    void hitTestFindsNearestCharacter() {
        const PageTextLayer layer = makeLayer();
        QCOMPARE(layer.charIndexNear(PointF{25.0, 106.0}, 0.0), 0);
        QCOMPARE(layer.charIndexNear(PointF{35.0, 106.0}, 0.0), 1);
        QCOMPARE(layer.charIndexNear(PointF{25.0, 86.0}, 0.0), 12);
        // 遠離所有字元時不得硬湊一個最近的出來，否則點空白處會選到整頁最後一個字。
        QCOMPARE(layer.charIndexNear(PointF{300.0, 300.0}, 2.0), -1);
    }

    void wordSelectionStopsAtBoundaries() {
        const PageTextLayer layer = makeLayer();
        QCOMPARE(layer.text(layer.wordRangeAt(1)), std::string("Hello"));
        QCOMPARE(layer.text(layer.wordRangeAt(5)), std::string(" "));
        QCOMPARE(layer.text(layer.wordRangeAt(8)), std::string("world"));
        // 換行不是詞，雙擊它不該選出任何東西。
        QVERIFY(layer.wordRangeAt(11).isEmpty());
        QVERIFY(layer.wordRangeAt(999).isEmpty());
    }

    void wordSelectionDoesNotCrossLines() {
        const PageTextLayer layer = makeLayer();
        // 第一行最後一個字元（index 10, 'd'）向後不得吃到第二行。
        const TextRange range = layer.wordRangeAt(10);
        QCOMPARE(layer.text(range), std::string("world"));
        QCOMPARE(range.end, 11);
    }

    void lineSelectionTrimsTrailingControlChars() {
        const PageTextLayer layer = makeLayer();
        const TextRange line0 = layer.lineRangeAt(3);
        QCOMPARE(layer.text(line0), std::string("Hello world"));
        QCOMPARE(layer.quads(line0).size(), std::size_t(1));

        const TextRange line1 = layer.lineRangeAt(12);
        QCOMPARE(layer.text(line1), std::string("second"));
        QCOMPARE(layer.lineCount(), 2);
    }

    void encodesCodePointsAsUtf8() {
        std::string out;
        appendUtf8(out, U'A');
        appendUtf8(out, 0x00E9);
        appendUtf8(out, 0x4F60);
        appendUtf8(out, 0x1F600);
        QCOMPARE(out, std::string("A\xC3\xA9\xE4\xBD\xA0\xF0\x9F\x98\x80"));
    }
};

QTEST_APPLESS_MAIN(TestTextLayer)
#include "test_text_layer.moc"
