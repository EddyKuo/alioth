// 從 Markdown 建立 PDF 的測試（PRD-IO-012，WBS 15）。
//
// 語法層的斷言查的是 parseMarkdown / layoutMarkdown 的結果，不是 PDF 位元組：
// 「這個字是不是粗體」在位元組上只表現為某個字型資源名，繞一圈去比對它
// 等於把測試綁死在資源命名上。位元組那一側由「能被 PDFium 開啟、頁數正確、
// qpdf 零警告」來守。

#include <QTemporaryDir>
#include <QtTest>

#include <string>

#include "create_test_support.h"
#include "domain/document_source.h"
#include "engine/create/markdown_to_pdf.h"

using namespace alioth::domain::create;
using namespace alioth::engine::create;

namespace {

// 關掉內容壓縮才看得到文字位元組。壓縮本身沒問題，但「內容有沒有寫進去」
// 這件事必須直接看得見，不能經過一層我們自己寫的解碼器再確認一次。
std::string buildUncompressed(const MarkdownLayout& layout) {
    PdfDocumentBuilder builder;
    builder.setCompressContent(false);
    appendMarkdownPages(builder, layout);
    const DocumentBuildResult built = builder.build();
    return built.ok ? built.bytes : std::string{};
}

bool hasRunWithFont(const MarkdownLayout& layout, const std::string& text, StandardFont font) {
    for (const auto& page : layout.pages) {
        for (const PlacedRun& run : page.runs) {
            if (run.text == text && run.font == font) return true;
        }
    }
    return false;
}

}  // namespace

class TestMarkdownImport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void parsesBlockKinds() {
        const std::string source =
            "# Title\n"
            "\n"
            "Some paragraph text.\n"
            "\n"
            "## Section\n"
            "\n"
            "- first\n"
            "- second\n"
            "  - nested\n"
            "\n"
            "1. one\n"
            "2. two\n"
            "\n"
            "```cpp\n"
            "int main() {}\n"
            "```\n"
            "\n"
            "---\n";

        const std::vector<MarkdownBlock> blocks = parseMarkdown(source);
        QCOMPARE(blocks.size(), std::size_t{10});
        QCOMPARE(blocks[0].kind, MarkdownBlockKind::Heading);
        QCOMPARE(blocks[0].headingLevel, 1);
        QCOMPARE(blocks[1].kind, MarkdownBlockKind::Paragraph);
        QCOMPARE(blocks[2].kind, MarkdownBlockKind::Heading);
        QCOMPARE(blocks[2].headingLevel, 2);
        QCOMPARE(blocks[3].kind, MarkdownBlockKind::ListItem);
        QCOMPARE(blocks[3].listDepth, 0);
        QCOMPARE(blocks[5].listDepth, 1);
        QCOMPARE(blocks[6].kind, MarkdownBlockKind::ListItem);
        QVERIFY(blocks[6].ordered);
        QCOMPARE(QString::fromStdString(blocks[6].marker), QStringLiteral("1."));
        QCOMPARE(blocks[8].kind, MarkdownBlockKind::CodeBlock);
        QCOMPARE(QString::fromStdString(blocks[8].codeLanguage), QStringLiteral("cpp"));
        QCOMPARE(blocks[8].codeLines.size(), std::size_t{1});
        QCOMPARE(blocks[9].kind, MarkdownBlockKind::HorizontalRule);
    }

    // h4 以下降級成 h3：再細分的字級差在 11pt 內文旁邊看不出來，
    // 而書籤層級太深反而難用。
    void deepHeadingsCollapseToLevelThree() {
        const std::vector<MarkdownBlock> blocks = parseMarkdown("#### deep\n##### deeper\n");
        QCOMPARE(blocks.size(), std::size_t{2});
        QCOMPARE(blocks[0].headingLevel, 3);
        QCOMPARE(blocks[1].headingLevel, 3);
    }

    void parsesInlineEmphasis() {
        const std::vector<MarkdownBlock> blocks =
            parseMarkdown("plain **bold** and *italic* and `code` end\n");
        QCOMPARE(blocks.size(), std::size_t{1});
        const auto& spans = blocks[0].spans;

        bool sawBold = false;
        bool sawItalic = false;
        bool sawCode = false;
        for (const InlineSpan& span : spans) {
            if (span.text == "bold") sawBold = span.bold;
            if (span.text == "italic") sawItalic = span.italic;
            if (span.text == "code") sawCode = span.code;
        }
        QVERIFY(sawBold);
        QVERIFY(sawItalic);
        QVERIFY(sawCode);
    }

    // 反斜線跳脫的星號必須印出來，不能當成強調標記。
    void backslashEscapesEmphasisMarker() {
        const std::vector<MarkdownBlock> blocks = parseMarkdown("a \\*b\\* c\n");
        QCOMPARE(blocks.size(), std::size_t{1});
        std::string joined;
        for (const InlineSpan& span : blocks[0].spans) {
            QVERIFY(!span.bold);
            QVERIFY(!span.italic);
            joined += span.text;
        }
        QCOMPARE(QString::fromStdString(joined), QStringLiteral("a *b* c"));
    }

    // 強調要對映到不同的標準 14 字型，否則畫出來全部一樣。
    void emphasisSelectsDistinctFonts() {
        const MarkdownLayout layout = layoutMarkdown("normal **bold** *slanted* `mono`\n");
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QVERIFY(hasRunWithFont(layout, "normal", StandardFont::Helvetica));
        QVERIFY(hasRunWithFont(layout, "bold", StandardFont::HelveticaBold));
        QVERIFY(hasRunWithFont(layout, "slanted", StandardFont::HelveticaOblique));
        QVERIFY(hasRunWithFont(layout, "mono", StandardFont::Courier));

        const std::string bytes = buildUncompressed(layout);
        QVERIFY(!bytes.empty());
        QVERIFY(bytes.find("/Helvetica-Bold") != std::string::npos);
        QVERIFY(bytes.find("/Helvetica-Oblique") != std::string::npos);
        QVERIFY(bytes.find("/Courier") != std::string::npos);
        QVERIFY(bytes.find("(bold) Tj") != std::string::npos);
    }

    // 程式碼區塊逐行原樣輸出，且用等寬字型——縮排是程式碼的語意。
    void codeBlockKeepsLinesVerbatim() {
        const MarkdownLayout layout = layoutMarkdown("```\n  indented\nplain\n```\n");
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        QVERIFY(hasRunWithFont(layout, "  indented", StandardFont::Courier));
        QVERIFY(hasRunWithFont(layout, "plain", StandardFont::Courier));
    }

    void horizontalRuleProducesAStrokedLine() {
        const MarkdownLayout layout = layoutMarkdown("above\n\n---\n\nbelow\n");
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        std::size_t rules = 0;
        for (const auto& page : layout.pages) rules += page.rules.size();
        QCOMPARE(rules, std::size_t{1});

        const std::string bytes = buildUncompressed(layout);
        QVERIFY(bytes.find(" m\n") != std::string::npos);
        QVERIFY(bytes.find(" l\nS\n") != std::string::npos);
    }

    // 巢狀清單的縮排必須真的往右，而不只是解析出 depth。
    void nestedListsIndentFurther() {
        const MarkdownLayout layout = layoutMarkdown("- outer\n  - inner\n");
        QVERIFY2(layout.ok, layout.diagnostic.c_str());
        double outerX = -1.0;
        double innerX = -1.0;
        for (const auto& page : layout.pages) {
            for (const PlacedRun& run : page.runs) {
                if (run.text == "outer") outerX = run.xPt;
                if (run.text == "inner") innerX = run.xPt;
            }
        }
        QVERIFY(outerX > 0.0);
        QVERIFY2(innerX > outerX, "巢狀清單沒有多縮排");
    }

    // 標題產生書籤意圖：標題文字、層級、所在頁。
    void headingsProduceBookmarkIntents() {
        const std::string source =
            "# Chapter One\n\nbody\n\n## Section A\n\nbody\n\n### Detail\n\nbody\n";
        const MarkdownImportResult result = createPdfFromMarkdown(source);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.bookmarks.size(), std::size_t{3});

        QCOMPARE(QString::fromStdString(result.bookmarks[0].title), QStringLiteral("Chapter One"));
        QCOMPARE(result.bookmarks[0].level, 1);
        QCOMPARE(result.bookmarks[0].pageIndex, 0);
        QCOMPARE(QString::fromStdString(result.bookmarks[1].title), QStringLiteral("Section A"));
        QCOMPARE(result.bookmarks[1].level, 2);
        QCOMPARE(result.bookmarks[2].level, 3);
        // 目的地必須在頁內，否則跳過去會落在頁面外。
        for (const BookmarkIntent& intent : result.bookmarks) {
            QVERIFY(intent.topYPt > 0.0);
            QVERIFY(intent.topYPt <= kPaperA4.heightPt);
        }
    }

    // 跨頁後書籤的頁碼要跟著走，否則書籤全部指向第一頁。
    void bookmarksTrackPageBreaks() {
        std::string source = "# First\n\n";
        for (int i = 0; i < 120; ++i) source += "Filler paragraph number " + std::to_string(i) + "\n\n";
        source += "# Second\n\ntail\n";

        const MarkdownImportResult result = createPdfFromMarkdown(source);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.pageCount > 1);
        QCOMPARE(result.bookmarks.size(), std::size_t{2});
        QCOMPARE(result.bookmarks[0].pageIndex, 0);
        QVERIFY2(result.bookmarks[1].pageIndex > 0, "第二個標題應該落在後面的頁");
        QVERIFY(result.bookmarks[1].pageIndex < static_cast<int>(result.pageCount));
    }

    // 表格語法不做版面，但也不吞掉：原樣當段落印出來，使用者看得見。
    void tableSyntaxFallsBackToParagraph() {
        const std::vector<MarkdownBlock> blocks = parseMarkdown("| a | b |\n| - | - |\n");
        QVERIFY(!blocks.empty());
        for (const MarkdownBlock& block : blocks) {
            QCOMPARE(block.kind, MarkdownBlockKind::Paragraph);
        }
    }

    void nonAsciiFailsExplicitly() {
        const MarkdownImportResult result = createPdfFromMarkdown("# \xE6\xA8\x99\xE9\xA1\x8C\n");
        QVERIFY(!result.ok);
        QVERIFY(result.nonAscii);
        QVERIFY(result.bytes.empty());
        QVERIFY(result.bookmarks.empty());
    }

    void outputOpensAndPassesQpdf() {
        const std::string source =
            "# Alioth\n"
            "\n"
            "A **portable** PDF review workstation with *tiled* rendering and `incremental` "
            "saving. This paragraph is deliberately long enough to force the greedy line "
            "breaker to produce several lines so that the wrapping path is exercised too.\n"
            "\n"
            "## Highlights\n"
            "\n"
            "- Tiled rendering\n"
            "- Incremental save\n"
            "  - Atomic rename\n"
            "1. First\n"
            "2. Second\n"
            "\n"
            "```\n"
            "qpdf --check out.pdf\n"
            "```\n"
            "\n"
            "***\n"
            "\n"
            "### Notes\n"
            "\n"
            "Escaped \\* stays literal, and (parentheses) must not break the stream.\n";

        const MarkdownImportResult result = createPdfFromMarkdown(source);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.pageCount >= 1);
        QCOMPARE(result.bookmarks.size(), std::size_t{3});

        const QString path = alioth::test::create::writeBytes(
            dir_->path(), QStringLiteral("markdown.pdf"), result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(static_cast<std::size_t>(opened.pageCount), result.pageCount);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("Markdown 匯入"));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestMarkdownImport)
#include "test_markdown_import.moc"
