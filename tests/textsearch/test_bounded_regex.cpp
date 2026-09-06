// 保證線性時間的正規表示式引擎測試（PRD-SRCH-002）。
//
// 最重要的性質不是「支援多少語法」而是「不會發生災難性回溯」——因此本測試
// 特別包含幾個對回溯引擎（如 std::regex）已知會指數爆炸的病態樣式，
// 用實際耗時上限驗證它們不會卡住，而不只是驗證結果正確。

#include <QtTest>

#include <string>

#include "engine/text/bounded_regex.h"

using namespace alioth::engine::text;

class TestBoundedRegex : public QObject {
    Q_OBJECT

private slots:
    void literalMatch() {
        const BoundedRegex re = BoundedRegex::compile("Alioth");
        QVERIFY(re.valid());
        const auto matches = re.findAll("Hello Alioth, welcome to Alioth again");
        QCOMPARE(matches.size(), std::size_t{2});
        QCOMPARE(matches[0].byteOffset, std::size_t{6});
        QCOMPARE(matches[0].byteLength, std::size_t{6});
        // "Hello Alioth, welcome to " 是 25 個位元組，第二個命中就從那裡開始。
        QCOMPARE(matches[1].byteOffset, std::size_t{25});
    }

    void caseInsensitiveOption() {
        const BoundedRegex re = BoundedRegex::compile("alioth", RegexOptions{.caseInsensitive = true});
        QVERIFY(re.valid());
        const auto matches = re.findAll("ALIOTH Alioth aLiOtH");
        QCOMPARE(matches.size(), std::size_t{3});
    }

    void digitClassAndPlus() {
        const BoundedRegex re = BoundedRegex::compile("\\d+");
        QVERIFY(re.valid());
        const auto matches = re.findAll("page 12 of 345, total 6");
        QCOMPARE(matches.size(), std::size_t{3});
        QCOMPARE(matches[0].byteLength, std::size_t{2});  // "12"
        QCOMPARE(matches[1].byteLength, std::size_t{3});  // "345"
        QCOMPARE(matches[2].byteLength, std::size_t{1});  // "6"
    }

    void alternationPicksFirstMatchingBranch() {
        const BoundedRegex re = BoundedRegex::compile("cat|dog|bird");
        QVERIFY(re.valid());
        const auto matches = re.findAll("I have a dog and a cat and a bird");
        QCOMPARE(matches.size(), std::size_t{3});
    }

    void anchorsRestrictToStartOrEnd() {
        const BoundedRegex start = BoundedRegex::compile("^Hello");
        QVERIFY(start.valid());
        QCOMPARE(start.findAll("Say Hello").size(), std::size_t{0});  // 不在開頭，不命中
        QCOMPARE(start.findAll("Hello there").size(), std::size_t{1});

        const BoundedRegex end = BoundedRegex::compile("end$");
        QVERIFY(end.valid());
        QCOMPARE(end.findAll("this is the end").size(), std::size_t{1});
        QCOMPARE(end.findAll("end of the road").size(), std::size_t{0});
    }

    void characterClassAndNegation() {
        const BoundedRegex vowels = BoundedRegex::compile("[aeiou]");
        QVERIFY(vowels.valid());
        QCOMPARE(vowels.findAll("hello").size(), std::size_t{2});

        const BoundedRegex nonDigits = BoundedRegex::compile("[^0-9]+");
        QVERIFY(nonDigits.valid());
        const auto matches = nonDigits.findAll("ab12cd34");
        QCOMPARE(matches.size(), std::size_t{2});  // "ab" 與 "cd"，結尾的 "34" 是數字不算
    }

    void quantifierRepeatRange() {
        const BoundedRegex re = BoundedRegex::compile("a{2,3}");
        QVERIFY(re.valid());
        // 貪婪：優先取到 3 個，"aaaa" 應該切成一次 3 個、剩一個 1 個不夠不命中，
        // 因此只有一個命中，長度 3。
        const auto matches = re.findAll("aaaa");
        QCOMPARE(matches.size(), std::size_t{1});
        QCOMPARE(matches[0].byteLength, std::size_t{3});
    }

    void unicodeByteOffsetsAreCorrect() {
        // "你好，Alioth" —— 中文字在 UTF-8 是 3 位元組，驗證命中位移是位元組數
        // 而不是碼點數。
        const BoundedRegex re = BoundedRegex::compile("Alioth");
        QVERIFY(re.valid());
        // 字面值刻意斷成兩段：C++ 的 \x 跳脫是貪婪的，"\x8CAlioth" 會被讀成
        // 一個超出範圍的 \x8CA，而不是 \x8C 後面接 "Alioth"。
        const std::string text = "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C" "Alioth";
        const auto matches = re.findAll(text);
        QCOMPARE(matches.size(), std::size_t{1});
        QCOMPARE(matches[0].byteOffset, std::size_t{9});  // 3 個全形字元 = 9 位元組
    }

    void rejectsBackreference() {
        const BoundedRegex re = BoundedRegex::compile("(a)\\1");
        QVERIFY(!re.valid());
        QVERIFY(!re.diagnostic().empty());
    }

    void rejectsNonGreedyQuantifier() {
        const BoundedRegex re = BoundedRegex::compile("a*?");
        QVERIFY(!re.valid());
    }

    void rejectsLookahead() {
        // (?=...) 不是本引擎支援的語法（'(' 後面直接接 '?' 不合法），
        // 编譯必須明確失敗，不能被誤解成別的意思。
        const BoundedRegex re = BoundedRegex::compile("foo(?=bar)");
        QVERIFY(!re.valid());
    }

    // 展開後的指令數超出安全上限：{m,n} 的重複次數本身不會造成回溯爆炸
    // （這個引擎不回溯），但仍然是一種資源耗盡手段，必須在編譯期擋下來。
    void rejectsOverlyLargeRepeatExpansion() {
        const BoundedRegex re = BoundedRegex::compile("a{1,100000}");
        QVERIFY(!re.valid());
        QVERIFY(!re.diagnostic().empty());
    }

    // 核心性質：對回溯引擎（std::regex、PCRE 未加保護時）已知的災難性回溯樣式，
    // 本引擎必須在合理時間內完成，即使答案是「沒有命中」。
    // (a+)+b 對抗回溯引擎的典型病態樣式：全 'a' 但缺結尾 'b'。
    void doesNotSufferCatastrophicBacktracking() {
        const BoundedRegex re = BoundedRegex::compile("(a+)+b");
        QVERIFY(re.valid());
        const std::string haystack(5000, 'a');  // 故意不含 'b'

        QElapsedTimer timer;
        timer.start();
        const auto matches = re.findAll(haystack);
        const qint64 elapsedMs = timer.elapsed();

        QVERIFY(matches.empty());
        // 500 毫秒是刻意寬鬆的上限：線性演算法對 5000 字元的樣式應該是毫秒等級，
        // 500ms 只是要抓「指數爆炸」而不是斤斤計較常數因子。
        QVERIFY2(elapsedMs < 500, qPrintable(QString("耗時 %1 毫秒，疑似發生了非線性行為").arg(elapsedMs)));
    }

    void doesNotSufferCatastrophicBacktrackingOnAlternation() {
        // (a|a)*c 是另一個經典的病態樣式：交替的兩個分支完全相同，
        // 對回溯引擎而言每個位置都要重複嘗試兩條路徑，組合數是指數的。
        const BoundedRegex re = BoundedRegex::compile("(a|a)*c");
        QVERIFY(re.valid());
        const std::string haystack(3000, 'a');

        QElapsedTimer timer;
        timer.start();
        const auto matches = re.findAll(haystack);
        const qint64 elapsedMs = timer.elapsed();

        QVERIFY(matches.empty());
        QVERIFY2(elapsedMs < 500, qPrintable(QString("耗時 %1 毫秒").arg(elapsedMs)));
    }

    void findFirstFromOffsetSkipsEarlierMatches() {
        const BoundedRegex re = BoundedRegex::compile("Alioth");
        QVERIFY(re.valid());
        const std::string text = "Alioth one, Alioth two";
        RegexMatch match{};
        QVERIFY(re.findFirst(text, 1, &match));
        QCOMPARE(match.byteOffset, std::size_t{12});
    }

    void emptyPatternIsInvalid() {
        const BoundedRegex re = BoundedRegex::compile("");
        QVERIFY(!re.valid());
    }
};

QTEST_APPLESS_MAIN(TestBoundedRegex)
#include "test_bounded_regex.moc"
