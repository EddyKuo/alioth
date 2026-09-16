// 從純文字建立 PDF 的測試（PRD-IO-012，WBS 15）。
//
// 斷行與分頁的斷言刻意直接查版面結果而不是查 PDF 位元組：位元組層面只能
// 看到「有幾個 Tj」，看不出某一行為什麼斷在那裡。位元組那一側改由
// PDFium 開得起來、頁數正確、qpdf 零警告來守。

#include <QTemporaryDir>
#include <QtTest>

#include <string>

#include "create_test_support.h"
#include "domain/document_source.h"
#include "engine/create/text_to_pdf.h"
#include "engine/fonts/cjk_font_library.h"

using namespace alioth::domain::create;
using namespace alioth::engine::create;

namespace {

std::string repeatLines(int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += "line " + std::to_string(i);
        if (i + 1 < count) text.push_back('\n');
    }
    return text;
}

}  // namespace

class TestTextImport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    // 每行都必須實際放得進版心。斷行只要偏寬一格，字就會印到紙外，
    // 而在螢幕上看起來只是「靠右一點」。
    void longParagraphWrapsWithinContentWidth() {
        std::string paragraph;
        for (int i = 0; i < 400; ++i) paragraph += "alpha bravo charlie delta ";

        TextImportOptions options;
        const TextLayout layout = layoutPlainText(paragraph, options);
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QVERIFY(layout.lineCount() > 20);

        const double contentWidth = options.paper.widthPt - options.marginPt * 2.0;
        for (const auto& page : layout.pages) {
            for (const std::string& line : page) {
                QVERIFY2(measureText(layout.font, line, layout.fontSize) <= contentWidth + 0.001,
                         line.c_str());
            }
        }
    }

    // 分頁數必須等於「行數 ÷ 每頁行數」的無條件進位，不是差不多。
    void pageCountMatchesLinesPerPage() {
        TextImportOptions options;
        const TextLayout layout = layoutPlainText(repeatLines(500), options);
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QCOMPARE(layout.lineCount(), std::size_t{500});

        const double contentHeight = options.paper.heightPt - options.marginPt * 2.0;
        const auto perPage = static_cast<std::size_t>(std::floor(contentHeight / layout.leading));
        QVERIFY(perPage > 1);
        const std::size_t expected = (500 + perPage - 1) / perPage;
        QCOMPARE(layout.pageCount(), expected);

        const TextImportResult result = createPdfFromPlainText(repeatLines(500), options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.pageCount, expected);

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("long.pdf"), result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(static_cast<std::size_t>(opened.pageCount), expected);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("純文字匯入"));
    }

    // 放不下一個「字」時逐字元硬切。長雜湊值與 URL 常常如此，
    // 讓它衝出頁面比切開更糟。
    void unbreakableWordIsSplitByCharacter() {
        const std::string blob(500, 'W');
        const TextLayout layout = layoutPlainText(blob);
        QVERIFY(layout.ok);
        QVERIFY(layout.lineCount() > 1);
        for (const auto& page : layout.pages) {
            for (const std::string& line : page) QVERIFY(!line.empty());
        }
    }

    // domain 的版面層只認 ASCII，並且要指出是第幾個位元組——那是使用者
    // 唯一能自救的資訊。內嵌字型的路徑在引擎層，見下一個測試。
    void asciiLayoutReportsOffendingByte() {
        const TextLayout layout = layoutPlainText("hello \xE4\xB8\xAD\xE6\x96\x87");
        QVERIFY(!layout.ok);
        QVERIFY2(layout.diagnostic.find("第 6 個位元組") != std::string::npos,
                 layout.diagnostic.c_str());
    }

    // 含 CJK 的輸入改走內嵌子集（ADR-007）。字型不在時仍必須明確失敗，
    // 不可退回拉丁字型畫出一排空框。
    void cjkTextEmbedsFontSubset() {
        const std::string text = "hello \xE4\xB8\xAD\xE6\x96\x87";
        const TextImportResult result = createPdfFromPlainText(text);

        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QVERIFY(!result.ok);
            QVERIFY(result.bytes.empty());
            QVERIFY2(result.diagnostic.find("內嵌字型缺少字形") != std::string::npos,
                     result.diagnostic.c_str());
            return;
        }

        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.pageCount, std::size_t{1});
        // CJK 字元必須走 /CJK 這個資源。落回 /F0（WinAnsiEncoding）不會報錯，
        // 只會畫出方框，而那在位元組層面看起來完全正常。
        QVERIFY(result.bytes.find("/CJK") != std::string::npos);

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("cjk.pdf"), result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("含 CJK 的純文字"));
    }

    // 壞掉的 UTF-8 不可以被 decoder 靜默補成替代字元後照樣輸出。
    void invalidUtf8FailsExplicitly() {
        const TextImportResult result = createPdfFromPlainText(std::string("hello \xFF\xFE"));
        QVERIFY(!result.ok);
        QVERIFY(result.nonAscii);
        QVERIFY2(result.diagnostic.find("有效的 UTF-8") != std::string::npos,
                 result.diagnostic.c_str());
        QVERIFY(result.bytes.empty());
    }

    void controlCharactersAreRejected() {
        const TextImportResult result = createPdfFromPlainText(std::string("a\x01\x62"));
        QVERIFY(!result.ok);
        QVERIFY(result.nonAscii);
    }

    // 括號與反斜線必須跳脫，否則其後所有物件會解析錯位。
    void parenthesesAreEscaped() {
        const TextImportResult result = createPdfFromPlainText("a (b) \\ c");
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.bytes.find("\\(b\\)") != std::string::npos);

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("escape.pdf"), result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("含括號的純文字"));
    }

    void monospaceUsesCourier() {
        TextImportOptions options;
        options.monospace = true;
        const TextImportResult result = createPdfFromPlainText("int main() { return 0; }", options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.bytes.find("/Courier") != std::string::npos);
        QVERIFY(result.bytes.find("/WinAnsiEncoding") != std::string::npos);
    }

    void tabsBecomeAlignedSpaces() {
        TextImportOptions options;
        options.tabWidth = 4;
        const TextLayout layout = layoutPlainText("ab\tcd", options);
        QVERIFY(layout.ok);
        QCOMPARE(QString::fromStdString(layout.pages[0][0]), QStringLiteral("ab  cd"));
    }

    void emptyInputStillProducesOnePage() {
        const TextImportResult result = createPdfFromPlainText("");
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.pageCount, std::size_t{1});
    }

    void oversizedMarginIsRejected() {
        TextImportOptions options;
        options.marginPt = 400.0;
        const TextImportResult result = createPdfFromPlainText("hello", options);
        QVERIFY(!result.ok);
        QVERIFY(!result.nonAscii);
    }

    // 換頁符（PRD-ANN-028「文件加摘要」用它把每一頁的摘要分開）。
    //
    // 不支援的話「每段摘要各自成頁」只能靠補空行湊，而空行數量取決於字級與
    // 紙張大小，換一個設定就全部錯開。
    void formFeedStartsANewPage() {
        const auto layout = alioth::domain::create::layoutPlainText("firstsecondthird");
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QCOMPARE(layout.pages.size(), std::size_t(3));
        QCOMPARE(QString::fromStdString(layout.pages[0][0]), QStringLiteral("first"));
        QCOMPARE(QString::fromStdString(layout.pages[1][0]), QStringLiteral("second"));
        QCOMPARE(QString::fromStdString(layout.pages[2][0]), QStringLiteral("third"));
    }

    void trailingFormFeedDoesNotAddABlankPage() {
        // 「最後一段結束」與「後面還有一個空頁」在使用者眼中完全不同，
        // 而多出來的空白頁是看得見的錯。
        const auto layout = alioth::domain::create::layoutPlainText("only");
        QVERIFY(layout.ok);
        QCOMPARE(layout.pages.size(), std::size_t(1));
    }

    void textWithoutFormFeedIsUnchanged() {
        const auto layout = alioth::domain::create::layoutPlainText("a\nb\nc");
        QVERIFY(layout.ok);
        QCOMPARE(layout.pages.size(), std::size_t(1));
        QCOMPARE(layout.pages[0].size(), std::size_t(3));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestTextImport)
#include "test_text_import.moc"
