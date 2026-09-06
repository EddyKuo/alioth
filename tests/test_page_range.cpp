// 頁碼範圍字串的解析（PRD-PAGE-002 / PAGE-004）。
//
// 這支解析器的每一個「錯誤處理」決定都會直接變成使用者的資料損失或困惑，
// 所以邊界條件比正常路徑重要得多：
//
//   「1,x,5」跳過壞片段 → 安靜地刪掉第 1 與第 5 頁，而使用者以為是三段
//   「1-999」夾到總頁數 → 變成「全部」，與輸入的意思完全不同
//   「3,3」不去重       → 刪除的數量與預期對不上

#include <QtTest>

#include "domain/page_range.h"

using alioth::domain::parsePageRange;

class TestPageRange : public QObject {
    Q_OBJECT

private slots:
    void singlePagesAndRangesBecomeZeroBased();
    void whitespaceIsTolerated();
    void reversedRangeIsAccepted();
    void duplicatesAreCollapsed();
    void oneBadFragmentVoidsTheWholeString();
    void outOfRangeVoidsInsteadOfClamping();
    void emptyInputYieldsNothing();
    void hugeNumbersDoNotOverflow();
};

void TestPageRange::singlePagesAndRangesBecomeZeroBased() {
    // 使用者輸入 1 起算，程式內部 0 起算——換算只在這裡做一次。
    QCOMPARE(parsePageRange("1,3,5-8", 10), (std::vector<int>{0, 2, 4, 5, 6, 7}));
    QCOMPARE(parsePageRange("1", 1), (std::vector<int>{0}));
}

void TestPageRange::whitespaceIsTolerated() {
    // 「1, 3 , 5」是很常見的打法，為此報錯只是刁難。
    QCOMPARE(parsePageRange(" 1 , 3 , 5 - 6 ", 10), (std::vector<int>{0, 2, 4, 5}));
}

void TestPageRange::reversedRangeIsAccepted() {
    // 「8-5」的意思很明確，沒有理由為此報錯。
    QCOMPARE(parsePageRange("8-5", 10), (std::vector<int>{4, 5, 6, 7}));
}

void TestPageRange::duplicatesAreCollapsed() {
    // 「3,3」幾乎一定是打字重複；不去重會讓刪除的數量與預期對不上。
    QCOMPARE(parsePageRange("3,3,2-3", 5), (std::vector<int>{1, 2}));
}

void TestPageRange::oneBadFragmentVoidsTheWholeString() {
    // **這是整支解析器最重要的一條。** 跳過壞片段會讓「1,x,5」安靜地
    // 刪掉第 1 與第 5 頁，而使用者以為自己輸入的是三段。
    QVERIFY(parsePageRange("1,x,5", 10).empty());
    QVERIFY(parsePageRange("1-", 10).empty());
    QVERIFY(parsePageRange("abc", 10).empty());
    QVERIFY(parsePageRange("1..3", 10).empty());
    QVERIFY(parsePageRange("-3", 10).empty());
}

void TestPageRange::outOfRangeVoidsInsteadOfClamping() {
    // 夾住的話「1-999」會變成「全部」，與輸入的意思完全不同。
    QVERIFY(parsePageRange("1-999", 10).empty());
    QVERIFY(parsePageRange("11", 10).empty());
    // 0 不是合法的頁碼（使用者輸入 1 起算）。
    QVERIFY(parsePageRange("0", 10).empty());
    QVERIFY(parsePageRange("0-2", 10).empty());
}

void TestPageRange::emptyInputYieldsNothing() {
    QVERIFY(parsePageRange("", 10).empty());
    QVERIFY(parsePageRange("   ", 10).empty());
    // 多打的逗號不是錯誤，只是多打了一個。
    QCOMPARE(parsePageRange("1,,2", 10), (std::vector<int>{0, 1}));
    // 沒有頁面時任何輸入都無效。
    QVERIFY(parsePageRange("1", 0).empty());
}

void TestPageRange::hugeNumbersDoNotOverflow() {
    // 十億以上直接判無效，而不是讓 int 溢位成負數再通過範圍檢查。
    QVERIFY(parsePageRange("99999999999", 10).empty());
    QVERIFY(parsePageRange("1-99999999999", 10).empty());
}

QTEST_APPLESS_MAIN(TestPageRange)
#include "test_page_range.moc"
