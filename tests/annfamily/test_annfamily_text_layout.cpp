// engine/annotations/text_layout.h 測試（WP24）。
//
// 這裡驗的是換行演算法本身的正確性與 PRD-ANN-030 Fit Box by Text Content
// 的高度計算，兩者都是可以精確驗算的純函數——「換行後的框高」正是
// 工作包說明裡點名要驗的項目，這裡用手算的期望值逐一核對，不是只驗「有換行」。

#include <QtTest>

#include <cmath>

#include "engine/annotations/text_layout.h"
#include "engine/fonts/cjk_font_library.h"

using namespace alioth::domain;
using namespace alioth::engine::annotations;

namespace {
bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

class TestAnnfamilyTextLayout : public QObject {
    Q_OBJECT

private slots:
    void estimateTextWidthMatchesHandComputedHelveticaMetrics() {
        // "AAAA"：四個大寫字母，每個 667/1000 em，字級 10 時應為 26.68pt。
        QVERIFY(near(estimateTextWidth("AAAA", 10.0), 26.68));
        // 空字串沒有寬度。
        QCOMPARE(estimateTextWidth("", 10.0), 0.0);
    }

    void shortTextIsNotWrapped() {
        TextFitOptions options{};
        options.fontSize = 10.0;
        options.maxWidth = 1000.0;  // 遠大於文字寬度，不應換行
        const TextLayoutResult result = layoutText("Hello World", options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.lines.size(), std::size_t{1});
        QCOMPARE(result.lines.front(), std::string("Hello World"));
    }

    void wrappingBreaksExactlyAtTheWordThatWouldOverflow() {
        // 兩個 "AAAA"（各 26.68pt）加一個空白（2.78pt）＝ 56.14pt。
        // maxWidth=40 小於合併寬度、大於單字寬度：必須斷成兩行。
        TextFitOptions options{};
        options.fontSize = 10.0;
        options.maxWidth = 40.0;
        const TextLayoutResult result = layoutText("AAAA BBBB", options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.lines.size(), std::size_t{2});
        QCOMPARE(result.lines[0], std::string("AAAA"));
        QCOMPARE(result.lines[1], std::string("BBBB"));

        // maxWidth=60 大於合併寬度：不應該斷行。
        TextFitOptions wide = options;
        wide.maxWidth = 60.0;
        const TextLayoutResult single = layoutText("AAAA BBBB", wide);
        QVERIFY(single.ok);
        QCOMPARE(single.lines.size(), std::size_t{1});
    }

    void explicitNewlinesAlwaysBreakRegardlessOfWidth() {
        TextFitOptions options{};
        options.fontSize = 10.0;
        options.maxWidth = 1000.0;
        const TextLayoutResult result = layoutText("Line1\nLine2\r\nLine3", options);
        QVERIFY(result.ok);
        QCOMPARE(result.lines.size(), std::size_t{3});
        QCOMPARE(result.lines[0], std::string("Line1"));
        QCOMPARE(result.lines[1], std::string("Line2"));
        QCOMPARE(result.lines[2], std::string("Line3"));
    }

    // ADR-007 之後，CJK 走內嵌的思源黑體子集而不是被拒絕。
    // 這一條的重點不是「成功」，而是**成功與失敗都要說得清楚**：
    // 有字型就畫得出來，沒字型就指名說沒有字型，缺字就指名是哪個碼點。
    // 任何一種情況都不可以靜默丟字——那會讓表單欄位的 /V 有值而畫面空白。
    void cjkTextLaysOutWhenAFontIsAvailable() {
        TextFitOptions options{};
        options.fontSize = 12.0;
        const TextLayoutResult result = layoutText("中文", options);

        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QVERIFY(!result.ok);
            QVERIFY2(result.diagnostic.find("CJK 字型") != std::string::npos,
                     result.diagnostic.c_str());
            return;
        }

        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.lines.size(), std::size_t(1));
        QCOMPARE(result.lines.front(), std::string("中文"));
        // 兩個全形字在 12pt 下應該接近 24pt 寬。用範圍而不是等值：
        // 實際字寬由字型決定，不同版本的思源黑體可能有微小差異。
        QVERIFY2(result.contentWidth > 18.0 && result.contentWidth < 30.0,
                 qPrintable(QStringLiteral("量到的寬度是 %1").arg(result.contentWidth)));
    }

    void cjkWrapsBetweenAnyTwoCharacters() {
        // 中文可以在任兩個字之間換行，拉丁文不行。把中文當成一個「詞」會讓
        // 一整段中文永遠是一行，直接衝出框外。
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        TextFitOptions options{};
        options.fontSize = 10.0;
        options.maxWidth = 25.0;  // 大約兩個全形字
        const TextLayoutResult result = layoutText("中文字型測試", options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY2(result.lines.size() > 1,
                 "一整段中文沒有換行——CJK 被當成單一個詞了");
        for (const std::string& line : result.lines) {
            QVERIFY(!line.empty());
            // 換行不可以切在 UTF-8 字元中間，否則會產生無效的位元組序列。
            QVERIFY2(line.size() % 3 == 0,
                     qPrintable(QStringLiteral("行「%1」的長度不是三的倍數，"
                                               "可能切在中文字中間")
                                    .arg(QString::fromStdString(line))));
        }
    }

    void cjkAndLatinJoinWithoutSpuriousSpaces() {
        // 中文字之間插空白會變成「中 文 字 距 很 寬」，那是很明顯的錯。
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        TextFitOptions options{};
        options.fontSize = 10.0;
        options.maxWidth = 500.0;
        const TextLayoutResult result = layoutText("中文", options);
        QVERIFY(result.ok);
        QCOMPARE(result.lines.front(), std::string("中文"));
    }

    void controlCharactersAreStillRefused() {
        TextFitOptions options{};
        options.fontSize = 12.0;
        const TextLayoutResult result = layoutText(std::string("ab"), options);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void zeroOrNegativeFontSizeIsRejected() {
        TextFitOptions options{};
        options.fontSize = 0.0;
        QVERIFY(!layoutText("x", options).ok);
    }

    // PRD-ANN-031：行距是使用者可調整的欄位（domain::FreeTextGeometry::
    // lineSpacing），appearance_stream 把它原封不動轉成這裡的
    // lineSpacingRatio；驗算方式與既有測試同一套：手算期望值逐一核對。
    void customLineSpacingRatioChangesLineHeight() {
        TextFitOptions options{};
        options.fontSize = 10.0;
        options.maxWidth = 1000.0;
        options.lineSpacingRatio = 2.0;
        const TextLayoutResult result = layoutText("Line1\nLine2", options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.lines.size(), std::size_t{2});
        // 行高 = fontSize * lineSpacingRatio = 10 * 2.0 = 20；
        // 內容高 = 2 行 * 20 = 40。
        QVERIFY(near(result.lineHeight, 20.0));
        QVERIFY(near(result.contentHeight, 40.0));
    }

    void fitBoxHeightMatchesHandComputedLineCount() {
        // 內縮 2pt 兩側，框寬 100pt → 可用寬度 96pt。
        // 每個 "AAAA" 寬 26.68pt、加空白 2.78pt：96pt 內最多塞 3 個字。
        // 7 個 "AAAA" 因此換成 3 / 3 / 1 共三行（見程式內註解的手算過程）。
        FitBoxOptions options{};
        options.fontSize = 10.0;
        options.paddingPt = 2.0;

        const RectF box{0.0, 500.0, 100.0, 600.0};
        const std::string text = "AAAA AAAA AAAA AAAA AAAA AAAA AAAA";
        const FitBoxResult fit = fitBoxByTextContent(box, text, options);
        QVERIFY2(fit.ok, fit.diagnostic.c_str());
        QCOMPARE(fit.layout.lines.size(), std::size_t{3});
        QCOMPARE(fit.layout.lines[0], std::string("AAAA AAAA AAAA"));
        QCOMPARE(fit.layout.lines[1], std::string("AAAA AAAA AAAA"));
        QCOMPARE(fit.layout.lines[2], std::string("AAAA"));

        // 行高 = 10 * 1.2 = 12；內容高 = 3 * 12 = 36；框高 = 36 + 2*2 = 40。
        QVERIFY(near(fit.layout.lineHeight, 12.0));
        QVERIFY(near(fit.layout.contentHeight, 36.0));

        // 錨點是左上角：left 與 top 不變，寬度沿用呼叫端給的 100，
        // 高度變成 40，因此 bottom = 600 - 40 = 560。
        QVERIFY(near(fit.rect.left, 0.0));
        QVERIFY(near(fit.rect.top, 600.0));
        QVERIFY(near(fit.rect.right, 100.0));
        QVERIFY(near(fit.rect.bottom, 560.0));
    }

    void fitBoxWithNoWidthFallsBackToNaturalSingleLineWidth() {
        // 沒有拖出寬度（width <= 0）時，框應該貼合單行文字的自然寬度，
        // 而不是換行——這是「使用者還沒決定寬度」的情境，Acrobat 的行為一致。
        FitBoxOptions options{};
        options.fontSize = 10.0;
        options.paddingPt = 2.0;

        const RectF box{0.0, 500.0, 0.0, 500.0};  // 寬 0
        const FitBoxResult fit = fitBoxByTextContent(box, "AAAA", options);
        QVERIFY2(fit.ok, fit.diagnostic.c_str());
        QCOMPARE(fit.layout.lines.size(), std::size_t{1});
        // 寬度 = 文字自然寬度 26.68 + 2*2 內縮 = 30.68。
        QVERIFY(near(fit.rect.width(), 30.68));
    }

    void fitBoxHandlesCjkContent() {
        FitBoxOptions options{};
        options.fontSize = 10.0;
        const RectF box{0.0, 0.0, 100.0, 50.0};
        const FitBoxResult fit = fitBoxByTextContent(box, "中文字", options);
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QVERIFY(!fit.ok);
            QVERIFY(!fit.diagnostic.empty());
            return;
        }
        QVERIFY2(fit.ok, fit.diagnostic.c_str());
        // 錨點是左上角：left 與 top 不動，只有高度依內容重算。
        QCOMPARE(fit.rect.left, box.left);
        QCOMPARE(fit.rect.top, box.top);
        QVERIFY(fit.rect.height() > 0.0);
    }

    // 固定框：高度不動，改成縮字（PRD-ANN-030 的另一半）。
    void fixedBoxShrinksTheFontInsteadOfGrowingTheBox() {
        FitBoxOptions options{};
        options.fontSize = 20.0;
        options.paddingPt = 2.0;
        options.maxHeight = 40.0;  // 只放得下兩行 20pt 字的一半

        const RectF box{0.0, 460.0, 100.0, 500.0};  // 高 40
        const std::string text = "AAAA AAAA AAAA AAAA AAAA AAAA AAAA AAAA";
        const FitBoxResult fit = fitBoxByTextContent(box, text, options);
        QVERIFY2(fit.ok, fit.diagnostic.c_str());

        // 框高完全不變——撐高會蓋掉使用者剛剛刻意避開的內容。
        QVERIFY(near(fit.rect.height(), 40.0));
        QVERIFY(near(fit.rect.top, 500.0));
        // 字級真的縮了，而且內容塞得進去。
        QVERIFY2(fit.fontSize < 20.0, "字級沒有縮小");
        QVERIFY(fit.fontSize >= options.minFontSize);
        QVERIFY2(fit.layout.contentHeight <= 40.0 - 2.0 * 2.0,
                 "縮字之後內容仍然超出可用高度");
        QVERIFY(!fit.overflows);
    }

    void fixedBoxThatAlreadyFitsKeepsTheRequestedFontSize() {
        // 塞得下就不該動字級。無條件縮一級會讓每一次重新貼合都變小一點，
        // 而使用者只是移動了一下框。
        FitBoxOptions options{};
        options.fontSize = 10.0;
        options.paddingPt = 2.0;
        options.maxHeight = 200.0;

        const RectF box{0.0, 300.0, 100.0, 500.0};
        const FitBoxResult fit = fitBoxByTextContent(box, "AAAA", options);
        QVERIFY2(fit.ok, fit.diagnostic.c_str());
        QVERIFY(near(fit.fontSize, 10.0));
        QVERIFY(!fit.overflows);
        QVERIFY(near(fit.rect.height(), 200.0));
    }

    void fixedBoxReportsOverflowInsteadOfShrinkingForever() {
        // 縮到 minFontSize 仍塞不下時要說出來。繼續縮下去只會得到一塊
        // 讀不了的灰影，而使用者不會知道那是「字太小」還是「畫壞了」。
        FitBoxOptions options{};
        options.fontSize = 12.0;
        options.paddingPt = 2.0;
        options.maxHeight = 12.0;   // 一行都放不下
        options.minFontSize = 6.0;

        std::string text;
        for (int i = 0; i < 60; ++i) text += "AAAA ";
        const RectF box{0.0, 488.0, 60.0, 500.0};
        const FitBoxResult fit = fitBoxByTextContent(box, text, options);
        QVERIFY2(fit.ok, fit.diagnostic.c_str());
        QVERIFY(near(fit.fontSize, options.minFontSize));
        QVERIFY2(fit.overflows, "塞不下卻沒有回報溢出——文字會被靜默裁掉");
    }

    void withoutMaxHeightTheBoxStillGrows() {
        // 沒有指定固定框時行為完全不變：高度跟著內容長，字級不動。
        FitBoxOptions options{};
        options.fontSize = 10.0;
        options.paddingPt = 2.0;

        const RectF box{0.0, 500.0, 100.0, 600.0};
        const FitBoxResult fit = fitBoxByTextContent(box, "AAAA AAAA AAAA AAAA", options);
        QVERIFY2(fit.ok, fit.diagnostic.c_str());
        QVERIFY(near(fit.fontSize, 10.0));
        QVERIFY(!fit.overflows);
        QVERIFY(near(fit.rect.height(), fit.layout.contentHeight + 4.0));
    }
};

QTEST_APPLESS_MAIN(TestAnnfamilyTextLayout)
#include "test_annfamily_text_layout.moc"
