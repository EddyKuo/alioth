// 文字層擷取測試（WBS 2.8，PRD-TXT-001 / 004）。
//
// 幾何斷言刻意寫成範圍而非精確值：字元寬度來自 PDFium 的內建 Helvetica 度量，
// 綁死小數點會讓 PDFium 升版時測試失敗，卻查不出是不是真的壞了。

#include <QtTest>

#include <cmath>
#include <string>
#include <vector>

#include "engine/text/text_extractor.h"
#include "pdf_fixture.h"
#include "text_pdf_fixture.h"

using namespace alioth::domain;
using namespace alioth::engine::text;

namespace {

// 把文字執行緒上的工作變成同步呼叫，斷言才能留在主執行緒。
// 正式程式碼不得這樣用：主執行緒單次阻塞上限是 16 毫秒。
template <typename Fn>
void onTextPage(TextExtractor& extractor, std::int32_t pageIndex, Fn&& fn) {
    extractor.withTextPage(pageIndex, [&fn](const TextPage* page) { fn(page); });
    extractor.waitForIdle();
}

std::int32_t indexOfSubstring(const PageTextLayer& layer, const std::string& needle) {
    for (std::int32_t i = 0; i + static_cast<std::int32_t>(needle.size()) <= layer.charCount(); ++i) {
        if (layer.text(TextRange::fromCount(i, static_cast<std::int32_t>(needle.size()))) == needle) {
            return i;
        }
    }
    return -1;
}

}  // namespace

class TestTextExtractor : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        file_ = alioth::test::writeTempPdf(alioth::test::makeTextPdf());
        QVERIFY2(file_ != nullptr, "無法建立測試用 PDF");

        DocumentError error = DocumentError::Unknown;
        extractor_.open(file_->fileName().toStdString(), "",
                        [&error](DocumentError e) { error = e; });
        extractor_.waitForIdle();
        QCOMPARE(error, DocumentError::None);
        QCOMPARE(extractor_.pageCount(), 2);
    }

    void reportsMissingFile() {
        TextExtractor extractor;
        DocumentError error = DocumentError::None;
        extractor.open("no-such-file-hopefully.pdf", "", [&error](DocumentError e) { error = e; });
        extractor.waitForIdle();
        QCOMPARE(error, DocumentError::FileNotFound);
    }

    void missingPageYieldsNoTextPage() {
        bool called = false;
        bool hadPage = true;
        onTextPage(extractor_, 9, [&](const TextPage* page) {
            called = true;
            hadPage = page != nullptr;
        });
        // 不得靜默吞噬：頁面不存在也要回呼，否則呼叫端永遠在等（IL-4）。
        QVERIFY(called);
        QVERIFY(!hadPage);
    }

    void countsCharsIncludingGeneratedBreaks() {
        std::int32_t count = 0;
        std::string text;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            count = charCount(*page);
            text = textForRange(*page, pageRange(*page));
        });

        // 三行可見字元共 63 個；PDFium 另外會生成換行字元，因此只驗下界。
        const std::int32_t visible = 12 + 19 + 32;
        QVERIFY2(count >= visible, qPrintable(QStringLiteral("字元數 %1 少於可見字元 %2")
                                                  .arg(count)
                                                  .arg(visible)));
        QVERIFY(text.find("Hello Alioth") != std::string::npos);
        QVERIFY(text.find("Second line of text") != std::string::npos);
        QVERIFY(text.find("Third line mentions Alioth again") != std::string::npos);
    }

    void charBoxesLandOnThePage() {
        std::vector<RectF> boxes;
        RectF firstCharBox{};
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            for (const TextChar& ch : page->layer().chars()) {
                if (ch.box.isEmpty()) continue;
                boxes.push_back(ch.box);
            }
            firstCharBox = charBox(*page, 0);
        });

        QVERIFY(!boxes.empty());
        for (const RectF& box : boxes) {
            QVERIFY2(box.left >= 0.0 && box.right <= alioth::test::kTextPageWidth,
                     "字元外框超出頁面寬度");
            QVERIFY2(box.bottom >= 0.0 && box.top <= alioth::test::kTextPageHeight,
                     "字元外框超出頁面高度");
            QVERIFY2(box.height() < 20.0, "12 點字的外框高度不合理");
        }

        // 第一行基線在 y = 400，字級 12 點；外框應貼著基線之上，而不是被翻到頁面上緣。
        QVERIFY2(firstCharBox.bottom >= 395.0 && firstCharBox.top <= 415.0,
                 "第一個字元不在基線附近——座標系被翻轉了");
        QVERIFY2(firstCharBox.left >= 48.0 && firstCharBox.left <= 60.0, "第一個字元不在左邊界");
    }

    void clickMapsToTheCharacterUnderIt() {
        std::string hit;
        std::int32_t missIndex = 0;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            const RectF box = charBox(*page, 0);
            const PointF centre{(box.left + box.right) * 0.5, (box.bottom + box.top) * 0.5};
            const std::int32_t index = charIndexAt(*page, centre, 2.0);
            if (index >= 0) hit = textForRange(*page, TextRange::fromCount(index, 1));
            // 頁面右下角是空白區，容忍度小於距離時不該命中任何字元。
            missIndex = charIndexAt(*page, PointF{380.0, 20.0}, 2.0);
        });

        QCOMPARE(hit, std::string("H"));
        QCOMPARE(missIndex, -1);
    }

    void selectionAcrossLinesProducesOneQuadPerLine() {
        std::vector<QuadPoint> quads;
        std::vector<QuadPoint> singleLineQuads;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            const PageTextLayer& layer = page->layer();
            const std::int32_t start = indexOfSubstring(layer, "Hello");
            const std::int32_t end = indexOfSubstring(layer, "Second line") + 11;
            QVERIFY(start >= 0 && end > start);
            quads = quadsForRange(*page, TextRange{start, end});
            singleLineQuads = quadsForRange(*page, TextRange::fromCount(start, 5));
        });

        QCOMPARE(singleLineQuads.size(), std::size_t(1));
        QVERIFY2(quads.size() >= 2,
                 "跨行選取必須切成多個 quad，否則螢光筆會蓋掉整塊矩形區域");

        for (const QuadPoint& quad : quads) {
            const RectF box = quad.boundingBox();
            QVERIFY(box.width() > 0.0);
            QVERIFY(box.height() > 0.0);
            // 四點順序：左上、右上、左下、右下。順序錯了 macOS 預覽會畫不出來。
            QVERIFY(quad.upperLeft.y >= quad.lowerLeft.y);
            QVERIFY(quad.upperRight.x >= quad.upperLeft.x);
        }
        // 兩個 quad 分屬不同行，垂直位置不得重疊。
        QVERIFY(quads[0].lowerLeft.y > quads[1].upperLeft.y);
    }

    void quadPointsSerialiseInSpecOrder() {
        const QuadPoint quad = QuadPoint::fromRect(RectF{10.0, 20.0, 30.0, 40.0});
        const std::vector<double> array = quad.toArray();
        QCOMPARE(array.size(), std::size_t(8));
        // ISO 32000-2 §12.5.6.10：x1 y1 x2 y2 x3 y3 x4 y4 = 左上 右上 左下 右下。
        const std::vector<double> expected{10.0, 40.0, 30.0, 40.0, 10.0, 20.0, 30.0, 20.0};
        QCOMPARE(array, expected);
    }

    void doubleClickSelectsWholeWord() {
        std::string word;
        std::string punctuationNeighbour;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            const std::int32_t index = indexOfSubstring(page->layer(), "Alioth");
            QVERIFY(index >= 0);
            word = textForRange(*page, wordRangeAt(*page, index + 2));
            // 詞的前一個字元是空白，雙擊空白只選到空白，不該吞掉旁邊的詞。
            punctuationNeighbour = textForRange(*page, wordRangeAt(*page, index - 1));
        });

        QCOMPARE(word, std::string("Alioth"));
        QCOMPARE(punctuationNeighbour, std::string(" "));
    }

    void tripleClickSelectsWholeLine() {
        std::string line;
        std::size_t quadCount = 0;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            const std::int32_t index = indexOfSubstring(page->layer(), "Second");
            QVERIFY(index >= 0);
            const TextRange range = lineRangeAt(*page, index);
            line = textForRange(*page, range);
            quadCount = quadsForRange(*page, range).size();
        });

        QCOMPARE(line, std::string("Second line of text"));
        QCOMPARE(quadCount, std::size_t(1));
    }

    void boundedTextReadsARectangularArea() {
        std::string firstLine;
        onTextPage(extractor_, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            firstLine = boundedText(*page, RectF{40.0, 396.0, 360.0, 414.0});
        });

        QVERIFY2(firstLine.find("Hello Alioth") != std::string::npos,
                 "區域文字沒讀到第一行——left/top/right/bottom 的參數順序可能傳錯了");
        QVERIFY2(firstLine.find("Second") == std::string::npos, "區域文字溢出到第二行");
    }

    void secondPageHasItsOwnTextLayer() {
        std::string text;
        onTextPage(extractor_, 1, [&](const TextPage* page) {
            QVERIFY(page != nullptr);
            text = textForRange(*page, pageRange(*page));
        });
        QVERIFY(text.find("Unique beta marker") != std::string::npos);
        QVERIFY(text.find("Second line of text") == std::string::npos);
    }

    // 旋轉頁的閱讀順序與座標（PRD-TXT-001）。
    //
    // /Rotate 只影響顯示，不改變頁面使用者空間裡的座標——CLAUDE.md「所有座標
    // 一律為未旋轉的頁面預設使用者空間」那條規則就是靠這個成立的。若擷取出來
    // 的座標跟著轉，選取框會與畫面差 90 度；而在未旋轉的頁面上永遠測不到，
    // 因為那時兩者剛好一樣。
    void rotatedPageKeepsUnrotatedTextCoordinates() {
        const auto upright = alioth::test::writeTempPdf(alioth::test::makeRotatedTextPdf(0));
        const auto rotated = alioth::test::writeTempPdf(alioth::test::makeRotatedTextPdf(90));
        QVERIFY(upright != nullptr && rotated != nullptr);

        // 量的是「Hello 的 H」那一個字元的框，不是索引 0 的字元。
        // 索引 0 在兩份文件裡是**不同的字**（順序會因 /Rotate 改變，見下），
        // 拿索引比對等於在比兩個不相干的字，那個測試永遠會紅得沒有意義。
        const auto measure = [](const QString& path, std::string& text, RectF& helloBox,
                                std::int32_t& count) {
            TextExtractor extractor;
            DocumentError error = DocumentError::Unknown;
            extractor.open(path.toStdString(), "", [&error](DocumentError e) { error = e; });
            extractor.waitForIdle();
            QCOMPARE(error, DocumentError::None);
            onTextPage(extractor, 0, [&](const TextPage* page) {
                QVERIFY(page != nullptr && page->valid());
                count = charCount(*page);
                text = textForRange(*page, pageRange(*page));
                const std::int32_t hello = indexOfSubstring(page->layer(), "Hello");
                QVERIFY(hello >= 0);
                helloBox = charBox(*page, hello);
            });
        };

        std::string uprightText;
        std::string rotatedText;
        RectF uprightBox{};
        RectF rotatedBox{};
        std::int32_t uprightCount = 0;
        std::int32_t rotatedCount = 0;
        measure(upright->fileName(), uprightText, uprightBox, uprightCount);
        measure(rotated->fileName(), rotatedText, rotatedBox, rotatedCount);

        // 字數不變、內容不少：轉頁不該讓任何一行消失。
        QCOMPARE(rotatedCount, uprightCount);
        QVERIFY(rotatedText.find("Hello Alioth") != std::string::npos);
        QVERIFY(rotatedText.find("Second line of text") != std::string::npos);

        // **順序會變，而且那是對的。**
        //
        // PDFium 依「顯示後的方向」決定閱讀順序：/Rotate 90 把原本在上方的
        // 那一行轉到右側，視覺上先讀到的是另一行。所以未旋轉頁是
        // 「Hello Alioth」在前，旋轉頁是「Second line of text」在前。
        //
        // 釘住這件事是因為它與下面的座標不變**同時**成立，而兩者看起來矛盾：
        // 順序照顯示走、座標照未旋轉的頁面走。呈現層據此分工——選取的錨點用
        // 座標，複製出來的文字用順序。哪天 PDFium 改了其中一邊，這裡會先紅。
        const std::size_t uprightHello = uprightText.find("Hello Alioth");
        const std::size_t uprightSecond = uprightText.find("Second line of text");
        const std::size_t rotatedHello = rotatedText.find("Hello Alioth");
        const std::size_t rotatedSecond = rotatedText.find("Second line of text");
        QVERIFY2(uprightHello < uprightSecond, "未旋轉頁的順序本身就不對");
        QVERIFY2(rotatedSecond < rotatedHello,
                 "旋轉頁的擷取順序沒有跟著顯示方向走——這一版 PDFium 換了行為，"
                 "複製旋轉頁的文字會與畫面上看到的順序不一致");

        // 座標也不變。差一個 90 度旋轉的話這裡的 left/bottom 會對調。
        QVERIFY2(std::abs(rotatedBox.left - uprightBox.left) < 0.01,
                 "同一個字在旋轉頁的 x 與未旋轉頁不同——座標跟著 /Rotate 轉了");
        QVERIFY2(std::abs(rotatedBox.bottom - uprightBox.bottom) < 0.01,
                 "同一個字在旋轉頁的 y 與未旋轉頁不同——座標跟著 /Rotate 轉了");
    }

    void rotatedPageHitTestingStillFindsTheCharacterUnderThePoint() {
        const auto rotated = alioth::test::writeTempPdf(alioth::test::makeRotatedTextPdf(270));
        QVERIFY(rotated != nullptr);

        TextExtractor extractor;
        DocumentError error = DocumentError::Unknown;
        extractor.open(rotated->fileName().toStdString(), "",
                       [&error](DocumentError e) { error = e; });
        extractor.waitForIdle();
        QCOMPARE(error, DocumentError::None);

        onTextPage(extractor, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr && page->valid());
            // 命中測試吃的是頁面座標，不是畫面座標——呈現層負責先把畫面座標
            // 經 PageTransform 轉回來。這裡直接餵頁面座標，驗的是引擎這一端
            // 沒有自己再轉一次。
            const RectF box = charBox(*page, 0);
            const PointF centre{(box.left + box.right) * 0.5, (box.bottom + box.top) * 0.5};
            const std::int32_t hit = charIndexAt(*page, centre, 2.0);
            QCOMPARE(hit, 0);
        });
    }

    // 直排（PRD-TXT-001）：文字往下堆疊時的閱讀順序與行判定。
    void verticalLayoutReadsTopToBottomAndDoesNotMergeIntoOneLine() {
        const auto vertical = alioth::test::writeTempPdf(alioth::test::makeVerticalTextPdf());
        QVERIFY(vertical != nullptr);

        TextExtractor extractor;
        DocumentError error = DocumentError::Unknown;
        extractor.open(vertical->fileName().toStdString(), "",
                       [&error](DocumentError e) { error = e; });
        extractor.waitForIdle();
        QCOMPARE(error, DocumentError::None);

        onTextPage(extractor, 0, [&](const TextPage* page) {
            QVERIFY(page != nullptr && page->valid());
            const std::string text = textForRange(*page, pageRange(*page));

            // 由上往下：A 在 B 前面，B 在 C 前面……順序反了代表擷取是按
            // 座標由下往上走的，那會讓直排文件的複製結果整段倒過來。
            std::string letters;
            for (const char c : text) {
                if (c >= 'A' && c <= 'E') letters.push_back(c);
            }
            QCOMPARE(letters, std::string("ABCDE"));

            // 每個字各自成行：把整條直行併成一行的話，三擊選行會選走整欄，
            // 而使用者按的是「選這一行」。
            const std::int32_t first = indexOfSubstring(page->layer(), "A");
            QVERIFY(first >= 0);
            const TextRange line = lineRangeAt(*page, first);
            QVERIFY2(line.count() <= 2,
                     "直排的一行被判成整欄——三擊選行會選走整條直行");
        });
    }

private:
    std::unique_ptr<QTemporaryFile> file_;
    TextExtractor extractor_;
};

QTEST_APPLESS_MAIN(TestTextExtractor)
#include "test_text_extractor.moc"
