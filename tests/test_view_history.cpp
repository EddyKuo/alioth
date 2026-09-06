// 瀏覽歷史的前進／後退（PRD-NAV-001）。
//
// 這組測試釘的多半是「按了沒反應」與「跳到奇怪的地方」這兩類缺陷。
// 它們不會崩潰、不會有錯誤訊息，只會讓使用者覺得這個功能不可靠而不再用它。

#include <QtTest>

#include "app/view_history.h"

using namespace alioth::app;

namespace {

ViewPosition at(int page, double scale = 1.0) {
    return ViewPosition{page, scale};
}

}  // namespace

class TestViewHistory : public QObject {
    Q_OBJECT

private slots:
    void emptyHistoryGoesNowhere() {
        ViewHistory history;
        QVERIFY(history.isEmpty());
        QVERIFY(!history.canGoBack());
        QVERIFY(!history.canGoForward());
        // 空歷史上按後退不可以崩潰，也不可以回傳垃圾。
        QCOMPARE(history.goBack().pageIndex, 0);
        QCOMPARE(history.goForward().pageIndex, 0);
    }

    void firstEntryIsNotSomethingToGoBackFrom() {
        // 只去過一個地方時「後退」應該是停用的。可以後退到哪裡？
        ViewHistory history;
        QVERIFY(history.record(at(5)));
        QCOMPARE(history.current().pageIndex, 5);
        QVERIFY(!history.canGoBack());
        QVERIFY(!history.canGoForward());
    }

    void backAndForwardWalkTheSamePath() {
        ViewHistory history;
        history.record(at(1));
        history.record(at(7));
        history.record(at(20));

        QCOMPARE(history.current().pageIndex, 20);
        QCOMPARE(history.goBack().pageIndex, 7);
        QCOMPARE(history.goBack().pageIndex, 1);
        QVERIFY(!history.canGoBack());
        QCOMPARE(history.goForward().pageIndex, 7);
        QCOMPARE(history.goForward().pageIndex, 20);
        QVERIFY(!history.canGoForward());
    }

    void repeatedJumpToTheSamePlaceIsNotRecordedTwice() {
        // 連按兩次同一個書籤之後，第一次按後退必須真的退到別的地方。
        // 少了這條防護，後退鍵會有一次「按了沒反應」。
        ViewHistory history;
        history.record(at(3));
        QVERIFY(history.record(at(9)));
        QVERIFY(!history.record(at(9)));
        QCOMPARE(history.size(), std::size_t(2));
        QCOMPARE(history.goBack().pageIndex, 3);
    }

    void sameeplaceStillUpdatesScale() {
        // 位置沒變但使用者縮放過：倍率要跟上，否則之後前進回來會用舊倍率。
        ViewHistory history;
        history.record(at(4, 1.0));
        history.record(at(4, 2.5));
        QCOMPARE(history.size(), std::size_t(1));
        QCOMPARE(history.current().scale, 2.5);
    }

    void newJumpFromTheMiddleDiscardsTheForwardBranch() {
        // 瀏覽器語意。保留前方紀錄會讓「前進」跳到一條使用者已經離開的分支上。
        ViewHistory history;
        history.record(at(1));
        history.record(at(2));
        history.record(at(3));
        history.goBack();  // 回到 2
        QVERIFY(history.canGoForward());

        history.record(at(50));
        QVERIFY(!history.canGoForward());
        QCOMPARE(history.current().pageIndex, 50);
        QCOMPARE(history.goBack().pageIndex, 2);
    }

    void goingBackThenForwardDoesNotDuplicateEntries() {
        ViewHistory history;
        history.record(at(1));
        history.record(at(2));
        history.goBack();
        history.goForward();
        QCOMPARE(history.size(), std::size_t(2));
    }

    void oldestEntriesFallOffAtTheCap() {
        ViewHistory history;
        for (int i = 0; i < static_cast<int>(ViewHistory::kMaxEntries) + 20; ++i) {
            history.record(at(i));
        }
        QCOMPARE(history.size(), ViewHistory::kMaxEntries);
        // 游標必須仍然指在最後一筆上。丟掉最舊的那筆時忘了移游標，
        // 症狀是「後退一次卻跳了兩格」。
        QCOMPARE(history.current().pageIndex,
                 static_cast<int>(ViewHistory::kMaxEntries) + 19);
        QVERIFY(history.canGoBack());
        QVERIFY(!history.canGoForward());
    }

    void clearResetsEverything() {
        ViewHistory history;
        history.record(at(1));
        history.record(at(2));
        history.clear();
        QVERIFY(history.isEmpty());
        QVERIFY(!history.canGoBack());
        QVERIFY(!history.canGoForward());
    }
};

QTEST_APPLESS_MAIN(TestViewHistory)
#include "test_view_history.moc"
