// 朗讀文字擷取測試（PRD-A11Y-006 的「可做的部分」，見標頭說明）。

#include <QtTest>

#include "engine/objects/reading_text_extractor.h"

using namespace alioth::engine::objects;

class TestReadingTextExtractor : public QObject {
    Q_OBJECT

private slots:
    void splitsOnAsciiAndCjkTerminators() {
        const auto sentences =
            splitIntoSentences(std::string("Hello world. Second sentence!") +
                              "\xE4\xB8\xAD\xE6\x96\x87\xE5\x8F\xA5\xE5\xAD\x90\xE3\x80\x82" +
                              "\xE7\x96\x91\xE5\x95\x8F\xE5\x8F\xA5\xEF\xBC\x9F");
        QCOMPARE(sentences.size(), std::size_t{4});
        QCOMPARE(QString::fromStdString(sentences[0]), QStringLiteral("Hello world."));
        QCOMPARE(QString::fromStdString(sentences[1]), QStringLiteral("Second sentence!"));
    }

    void splitTrimsWhitespaceAndDropsEmptySegments() {
        const auto sentences = splitIntoSentences("  One.   \n\n Two.  ");
        QCOMPARE(sentences.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(sentences[0]), QStringLiteral("One."));
        QCOMPARE(QString::fromStdString(sentences[1]), QStringLiteral("Two."));
    }

    void usesActualTextFirst() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode span;
        span.type = "Span";
        span.pageIndex = 0;
        span.actualText = "真正要念的文字。";
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(span));
        tree.roots.push_back(std::move(doc));

        const ReadingExtractionResult result = extractReadingText(tree, 0);
        QCOMPARE(result.utterances.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(result.utterances[0].text),
                 QStringLiteral("真正要念的文字。"));
        QCOMPARE(result.skippedNoTextCount, 0);
        QCOMPARE(result.skippedMissingAltCount, 0);
    }

    void figureWithoutAltIsSkippedAndCounted() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode figure;
        figure.type = "Figure";
        figure.pageIndex = 0;
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(figure));
        tree.roots.push_back(std::move(doc));

        const ReadingExtractionResult result = extractReadingText(tree, 0);
        QVERIFY(result.utterances.empty());
        QCOMPARE(result.skippedMissingAltCount, 1);
        // 這是「缺陷」而不是「裝飾性內容」，因此不該混進一般的 skippedNoTextCount，
        // 兩個計數要能分開回報給使用者。
        QCOMPARE(result.skippedNoTextCount, 0);
    }

    void figureWithAltIsNarrated() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode figure;
        figure.type = "Figure";
        figure.pageIndex = 0;
        figure.altText = "流程圖";
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(figure));
        tree.roots.push_back(std::move(doc));

        const ReadingExtractionResult result = extractReadingText(tree, 0);
        QCOMPARE(result.utterances.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(result.utterances[0].text), QStringLiteral("流程圖"));
    }

    void plainParagraphWithoutActualTextIsSkippedAsUnavailable() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode paragraph;  // 一般 /P，沒有 /ActualText——這是本模組誠實承認擷取不到的情況。
        paragraph.type = "P";
        paragraph.pageIndex = 0;
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(paragraph));
        tree.roots.push_back(std::move(doc));

        const ReadingExtractionResult result = extractReadingText(tree, 0);
        QVERIFY(result.utterances.empty());
        QCOMPARE(result.skippedNoTextCount, 1);
        QCOMPARE(result.skippedMissingAltCount, 0);
    }

    void headingUsesTitleAsFallback() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode heading;
        heading.type = "H1";
        heading.pageIndex = 0;
        heading.title = "第一章";
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(heading));
        tree.roots.push_back(std::move(doc));

        const ReadingExtractionResult result = extractReadingText(tree, 0);
        QCOMPARE(result.utterances.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(result.utterances[0].text), QStringLiteral("第一章"));
    }

    void otherPagesAreExcluded() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode span;
        span.type = "Span";
        span.pageIndex = 1;  // 第二頁
        span.actualText = "不該出現在第一頁的朗讀結果";
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(span));
        tree.roots.push_back(std::move(doc));

        const ReadingExtractionResult result = extractReadingText(tree, 0);
        QVERIFY(result.utterances.empty());
    }
};

QTEST_APPLESS_MAIN(TestReadingTextExtractor)
#include "test_reading_text_extractor.moc"
