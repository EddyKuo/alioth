// 字數統計測試（PRD-UI-019，R2——見 text_statistics.h 開頭的偏離說明）。
//
// 重點：CJK 沒有空白分詞，逐字算是唯一合理辦法；西文以空白/標點斷詞；
// 字數與字元數是兩個不同的量，不能互相替代。

#include <QtTest>

#include "app/uisystem/text_statistics.h"

using namespace alioth::app;

class TestTextStatistics : public QObject {
    Q_OBJECT

private slots:
    void latinWordsCountBySpaceDelimitedRuns() {
        const auto stats = computeTextStatistics(QStringLiteral("Hello world, this is Alioth."));
        QCOMPARE(stats.wordCount, 5LL);  // Hello / world / this / is / Alioth
        QCOMPARE(stats.latinWordCount, 5LL);
        QCOMPARE(stats.cjkCharacterCount, 0LL);
    }

    void cjkCountsEachCharacterAsOneWord() {
        const auto stats = computeTextStatistics(QStringLiteral("跨平台專業PDF審閱工作站"));
        // 中文字：跨平台專業審閱工作站 = 10 個表意字；PDF 是三個西文字母連續字元算一詞。
        QCOMPARE(stats.cjkCharacterCount, 10LL);
        QCOMPARE(stats.latinWordCount, 1LL);
        QCOMPARE(stats.wordCount, 11LL);
    }

    void mixedTextSeparatesCjkAndLatinCorrectly() {
        const auto stats = computeTextStatistics(QStringLiteral("這是 Alioth 的 PDF 檢視器"));
        // CJK 字：這是的檢視器 = 6；Latin 詞：Alioth, PDF = 2
        QCOMPARE(stats.cjkCharacterCount, 6LL);
        QCOMPARE(stats.latinWordCount, 2LL);
        QCOMPARE(stats.wordCount, 8LL);
    }

    void characterCountWithAndWithoutSpacesDiffer() {
        const auto stats = computeTextStatistics(QStringLiteral("a b c"));
        QCOMPARE(stats.characterCountWithSpaces, 5LL);   // a, ' ', b, ' ', c
        QCOMPARE(stats.characterCountNoSpaces, 3LL);      // a, b, c
    }

    void newlinesAreNotCountedAsVisibleCharacters() {
        const auto stats = computeTextStatistics(QStringLiteral("a\nb\r\nc"));
        QCOMPARE(stats.characterCountWithSpaces, 3LL);  // a, b, c — 換行不算可見字元
        QCOMPARE(stats.wordCount, 3LL);
    }

    void emptyStringYieldsZeroStatistics() {
        const auto stats = computeTextStatistics(QString());
        QCOMPARE(stats.wordCount, 0LL);
        QCOMPARE(stats.characterCountWithSpaces, 0LL);
        QCOMPARE(stats.characterCountNoSpaces, 0LL);
    }

    void perPageStatisticsAccumulateForWholeDocument() {
        const auto page1 = computeTextStatistics(QStringLiteral("第一頁"));
        const auto page2 = computeTextStatistics(QStringLiteral("第二頁"));
        const auto total = page1 + page2;
        QCOMPARE(total.cjkCharacterCount, page1.cjkCharacterCount + page2.cjkCharacterCount);
        QCOMPARE(total.wordCount, 6LL);
    }
};

QTEST_GUILESS_MAIN(TestTextStatistics)
#include "test_text_statistics.moc"
