// Token 差異演算法（PRD-CMP-001 的核心）。
//
// diff 的錯誤有兩種，嚴重程度差很多：多報一處差異只是雜訊，**少報一處是漏掉了
// 使用者需要看到的變更**，而使用者不會知道自己漏看了什麼。所以這裡的斷言偏向
// 「編輯段必須完整覆蓋兩側序列」——那是不漏報的結構性保證。

#include <QtTest>

#include <numeric>
#include <string>
#include <vector>

#include "engine/compare/token_diff.h"

using namespace alioth;
using namespace alioth::engine::compare;

namespace {

// 以空白切詞後配號。測試裡直接寫句子比寫 id 陣列可讀得多。
std::vector<TokenId> tokenize(TokenTable& table, const std::string& text) {
    std::vector<TokenId> ids;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(' ', start);
        const std::string word =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!word.empty()) ids.push_back(table.intern(word));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return ids;
}

// 編輯段必須完整覆蓋兩側，且不重疊。這是「不漏報」的結構性檢查：
// 只要有一段沒被覆蓋到，那段的差異就永遠不會被顯示出來。
void verifySpansCoverBothSides(const TokenDiff& diff, std::size_t oldSize, std::size_t newSize) {
    std::int32_t oldCursor = 0;
    std::int32_t newCursor = 0;
    for (const domain::EditSpan& span : diff.spans) {
        QCOMPARE(span.oldSpan.start, oldCursor);
        QCOMPARE(span.newSpan.start, newCursor);
        QVERIFY(span.oldSpan.end >= span.oldSpan.start);
        QVERIFY(span.newSpan.end >= span.newSpan.start);
        oldCursor = span.oldSpan.end;
        newCursor = span.newSpan.end;
    }
    QCOMPARE(oldCursor, static_cast<std::int32_t>(oldSize));
    QCOMPARE(newCursor, static_cast<std::int32_t>(newSize));
}

}  // namespace

class TestTokenDiff : public QObject {
    Q_OBJECT

private slots:
    void identicalSequencesReportNoChanges() {
        TokenTable table;
        const auto a = tokenize(table, "the quick brown fox");
        const auto b = tokenize(table, "the quick brown fox");

        const TokenDiff diff = diffTokens(a, b);
        QCOMPARE(diff.status, DiffStatus::Ok);
        QVERIFY(!diff.hasChanges());
        QCOMPARE(diff.commonCount(), 4);
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void completelyDifferentSequences() {
        TokenTable table;
        const auto a = tokenize(table, "alpha beta gamma");
        const auto b = tokenize(table, "delta epsilon zeta");

        const TokenDiff diff = diffTokens(a, b);
        QCOMPARE(diff.status, DiffStatus::Ok);
        QVERIFY(diff.hasChanges());
        QCOMPARE(diff.commonCount(), 0);
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void pureInsertion() {
        TokenTable table;
        const auto a = tokenize(table, "one two five");
        const auto b = tokenize(table, "one two three four five");

        const TokenDiff diff = diffTokens(a, b);
        QVERIFY(diff.hasChanges());
        // 共同的三個詞必須被認出來，否則整段會被報成替換，
        // 使用者看到的是「整頁都改了」而不是「插入了兩個詞」。
        QCOMPARE(diff.commonCount(), 3);
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void pureDeletion() {
        TokenTable table;
        const auto a = tokenize(table, "one two three four five");
        const auto b = tokenize(table, "one five");

        const TokenDiff diff = diffTokens(a, b);
        QVERIFY(diff.hasChanges());
        QCOMPARE(diff.commonCount(), 2);
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void insertionInTheMiddleKeepsBothEnds() {
        TokenTable table;
        const auto a = tokenize(table, "head body tail");
        const auto b = tokenize(table, "head body inserted tail");

        const TokenDiff diff = diffTokens(a, b);
        QCOMPARE(diff.commonCount(), 3);
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void emptyInputsOnBothSides() {
        const TokenDiff diff = diffTokens({}, {});
        QCOMPARE(diff.status, DiffStatus::Ok);
        QVERIFY(!diff.hasChanges());
        QCOMPARE(diff.commonCount(), 0);
    }

    void emptyAgainstNonEmpty() {
        TokenTable table;
        const auto b = tokenize(table, "only new content");

        const TokenDiff insertOnly = diffTokens({}, b);
        QVERIFY(insertOnly.hasChanges());
        QCOMPARE(insertOnly.commonCount(), 0);
        verifySpansCoverBothSides(insertOnly, 0, b.size());

        const TokenDiff deleteOnly = diffTokens(b, {});
        QVERIFY(deleteOnly.hasChanges());
        verifySpansCoverBothSides(deleteOnly, b.size(), 0);
    }

    void singleTokenSequences() {
        TokenTable table;
        const auto a = tokenize(table, "same");
        const auto b = tokenize(table, "same");
        QVERIFY(!diffTokens(a, b).hasChanges());

        const auto c = tokenize(table, "other");
        QVERIFY(diffTokens(a, c).hasChanges());
    }

    void repeatedTokensAreNotCollapsed() {
        // 「同一個詞出現五次」與「出現一次」在文件裡是不同的內容。
        TokenTable table;
        const auto a = tokenize(table, "x x x x x");
        const auto b = tokenize(table, "x x");

        const TokenDiff diff = diffTokens(a, b);
        QVERIFY(diff.hasChanges());
        QCOMPARE(diff.commonCount(), 2);
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void oversizedInputIsRejectedNotDegraded() {
        // 兩份都是不可信任輸入。超出上限要明確拒絕，而不是跑到記憶體耗盡。
        DiffLimits limits;
        limits.maxTokensPerSide = 8;

        std::vector<TokenId> big(20);
        std::iota(big.begin(), big.end(), TokenId{1});

        const TokenDiff diff = diffTokens(big, big, limits);
        QCOMPARE(diff.status, DiffStatus::InputTooLarge);
        QVERIFY(!diff.ok());
        QVERIFY(diff.spans.empty());
    }

    void costLimitDegradesButStillCoversBothSides() {
        // 超出時間上限時結果仍然必須正確覆蓋兩側，只是不保證最小編輯。
        // 「降級」不等於「可以少報」。
        DiffLimits limits;
        limits.maxCost = 1;

        TokenTable table;
        const auto a = tokenize(table, "a b c d e f g h");
        const auto b = tokenize(table, "h g f e d c b a");

        const TokenDiff diff = diffTokens(a, b, limits);
        QVERIFY(diff.ok());
        verifySpansCoverBothSides(diff, a.size(), b.size());
    }

    void tokenTableAssignsStableIds() {
        TokenTable table;
        const TokenId first = table.intern("word");
        QCOMPARE(table.intern("word"), first);
        QVERIFY(table.intern("other") != first);

        // lookup 不得污染登錄表——用於「查詢但不配號」的場合。
        const std::size_t before = table.size();
        QCOMPARE(table.lookup("never-seen"), TokenTable::kUnknown);
        QCOMPARE(table.size(), before);
        QCOMPARE(table.lookup("word"), first);
    }
};

QTEST_APPLESS_MAIN(TestTokenDiff)
#include "test_token_diff.moc"
