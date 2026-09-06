// 文件歷史與使用者書籤（PRD-NAV-005、PRD-NAV-009）。
//
// 這支測試打的是純資料層。刻意不碰 QSettings：那條路徑會污染測試機器的
// 使用者設定，而且在 CI 上是共用狀態，會讓平行執行的測試互相干擾。

#include <QtTest>

#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "app/history_store.h"

using namespace alioth;
using app::HistoryStore;
using app::ReadingMark;

class TestHistoryStore : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        for (const QString& name : {QStringLiteral("a.pdf"), QStringLiteral("b.pdf"),
                                    QStringLiteral("c.pdf")}) {
            QFile file(dir_->filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("%PDF-1.7\n");
        }
    }

    void recordOpenPutsNewestFirst() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        store.recordOpen(path("b.pdf"), QStringLiteral("B"), 20);

        QCOMPARE(store.entries().size(), std::size_t{2});
        QCOMPARE(store.entries()[0].title, QStringLiteral("B"));
    }

    void reopeningMovesToFrontWithoutDuplicating() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        store.recordOpen(path("b.pdf"), QStringLiteral("B"), 20);
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);

        QCOMPARE(store.entries().size(), std::size_t{2});
        QCOMPARE(store.entries()[0].title, QStringLiteral("A"));
    }

    void reopeningKeepsExistingMarksAndPosition() {
        // 重新開啟同一份文件不該把使用者的書籤洗掉——那是最容易寫錯的地方，
        // 因為「更新記錄」最直覺的寫法就是整筆覆蓋。
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        QVERIFY(store.updatePosition(path("a.pdf"), 7, 1.5));
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{3, QStringLiteral("重點"), {}}));

        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);

        const app::HistoryEntry* entry = store.find(path("a.pdf"));
        QVERIFY(entry != nullptr);
        QCOMPARE(entry->lastPageIndex, 7);
        QCOMPARE(entry->marks.size(), std::size_t{1});
        QCOMPARE(entry->marks[0].label, QStringLiteral("重點"));
    }

    void differentSpellingsOfSamePathCollapse() {
        // 從命令列給相對路徑、從檔案總管給絕對路徑，是同一份檔案。
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        store.recordOpen(dir_->path() + QStringLiteral("/./a.pdf"), QStringLiteral("A"), 10);
        QCOMPARE(store.entries().size(), std::size_t{1});
    }

#ifdef _WIN32
    void windowsPathsAreCaseInsensitive() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        store.recordOpen(path("a.pdf").toUpper(), QStringLiteral("A"), 10);
        QCOMPARE(store.entries().size(), std::size_t{1});
    }
#endif

    void positionOutsideDocumentIsRejected() {
        // 存進去的話症狀會出現在下次開檔時，離成因很遠。
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        QVERIFY(!store.updatePosition(path("a.pdf"), 10, 1.0));
        QVERIFY(!store.updatePosition(path("a.pdf"), -1, 1.0));
        QVERIFY(!store.updatePosition(path("a.pdf"), 5, 0.0));
        QVERIFY(store.updatePosition(path("a.pdf"), 9, 1.0));
        QCOMPARE(store.find(path("a.pdf"))->lastPageIndex, 9);
    }

    void positionForUnknownDocumentIsNotCreated() {
        HistoryStore store;
        QVERIFY(!store.updatePosition(path("a.pdf"), 1, 1.0));
        QVERIFY(store.entries().empty());
    }

    void marksStaySortedByPageAndDeduplicate() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 100);
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{50, {}, {}}));
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{10, {}, {}}));
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{30, {}, {}}));
        // 同一頁再加一次是「改標籤」，不是第二個書籤。
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{30, QStringLiteral("改過"), {}}));

        const std::vector<ReadingMark> marks = store.marks(path("a.pdf"));
        QCOMPARE(marks.size(), std::size_t{3});
        QCOMPARE(marks[0].pageIndex, 10);
        QCOMPARE(marks[1].pageIndex, 30);
        QCOMPARE(marks[1].label, QStringLiteral("改過"));
        QCOMPARE(marks[2].pageIndex, 50);
    }

    void markOutsideDocumentIsRejected() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 5);
        QVERIFY(!store.addMark(path("a.pdf"), ReadingMark{5, {}, {}}));
        QVERIFY(!store.addMark(path("a.pdf"), ReadingMark{-1, {}, {}}));
    }

    void removeMarkReportsWhetherItExisted() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 10);
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{2, {}, {}}));
        QVERIFY(store.removeMark(path("a.pdf"), 2));
        QVERIFY(!store.removeMark(path("a.pdf"), 2));
    }

    void entryCountIsBounded() {
        HistoryStore store;
        for (int i = 0; i < HistoryStore::kMaxEntries + 20; ++i) {
            store.recordOpen(dir_->filePath(QStringLiteral("gone-%1.pdf").arg(i)),
                             QStringLiteral("T%1").arg(i), 1);
        }
        QCOMPARE(static_cast<int>(store.entries().size()), HistoryStore::kMaxEntries);
        // 丟掉的是最舊的那些，最新的必須還在。
        QCOMPARE(store.entries()[0].title,
                 QStringLiteral("T%1").arg(HistoryStore::kMaxEntries + 19));
    }

    void missingFileIsKeptButFlagged() {
        // 隨身碟拔掉時，使用者仍然需要靠標題想起那份文件是什麼。
        HistoryStore store;
        store.recordOpen(dir_->filePath(QStringLiteral("never-existed.pdf")),
                         QStringLiteral("已不在"), 3);
        QCOMPARE(store.entries().size(), std::size_t{1});
        QVERIFY(!store.entries()[0].fileExists());

        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 3);
        QVERIFY(store.find(path("a.pdf"))->fileExists());
    }

    void searchMatchesTitleAndPathCaseInsensitively() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("結構計算書"), 1);
        store.recordOpen(path("b.pdf"), QStringLiteral("Mechanical Spec"), 1);

        QCOMPARE(store.search(QStringLiteral("計算")).size(), std::size_t{1});
        QCOMPARE(store.search(QStringLiteral("mechanical")).size(), std::size_t{1});
        QCOMPARE(store.search(QStringLiteral("b.pdf")).size(), std::size_t{1});
        QCOMPARE(store.search(QString()).size(), std::size_t{2});
    }

    void jsonRoundTripPreservesEverything() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("含 \"引號\" 與,逗號"), 40);
        QVERIFY(store.updatePosition(path("a.pdf"), 12, 2.25));
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{4, QStringLiteral("第一處"), {}}));
        QVERIFY(store.addMark(path("a.pdf"), ReadingMark{9, {}, {}}));
        store.recordOpen(path("b.pdf"), QStringLiteral("B"), 5);

        const QByteArray raw = QJsonDocument(store.toJson()).toJson(QJsonDocument::Compact);
        const HistoryStore restored =
            HistoryStore::fromJson(QJsonDocument::fromJson(raw).object());

        QCOMPARE(restored.entries().size(), std::size_t{2});
        const app::HistoryEntry* entry = restored.find(path("a.pdf"));
        QVERIFY(entry != nullptr);
        QCOMPARE(entry->title, QStringLiteral("含 \"引號\" 與,逗號"));
        QCOMPARE(entry->lastPageIndex, 12);
        QCOMPARE(entry->lastScale, 2.25);
        QCOMPARE(entry->pageCount, 40);
        QCOMPARE(entry->marks.size(), std::size_t{2});
        QCOMPARE(entry->marks[0].label, QStringLiteral("第一處"));
        QVERIFY(entry->marks[0].createdAt.isValid());
    }

    void corruptEntriesAreSkippedNotFatal() {
        // 歷史是輔助資料。為了一筆爛記錄丟掉其餘全部，代價遠大於收益。
        const QByteArray raw = QByteArrayLiteral(
            "{\"version\":1,\"entries\":["
            "{\"title\":\"沒有路徑\"},"
            "{\"path\":\"C:/tmp/ok.pdf\",\"title\":\"好的\",\"lastPage\":2,\"lastScale\":-3,"
            "\"marks\":[{\"label\":\"沒有頁碼\"},{\"page\":1}]}"
            "]}");
        const HistoryStore store = HistoryStore::fromJson(QJsonDocument::fromJson(raw).object());

        QCOMPARE(store.entries().size(), std::size_t{1});
        QCOMPARE(store.entries()[0].title, QStringLiteral("好的"));
        // 負的縮放比例是壞資料，退回 1.0 而不是照單全收。
        QCOMPARE(store.entries()[0].lastScale, 1.0);
        QCOMPARE(store.entries()[0].marks.size(), std::size_t{1});
        QCOMPARE(store.entries()[0].marks[0].pageIndex, 1);
    }

    void fromJsonSortsByRecency() {
        const QByteArray raw = QByteArrayLiteral(
            "{\"entries\":["
            "{\"path\":\"C:/tmp/old.pdf\",\"title\":\"舊\",\"lastOpened\":\"2020-01-01T00:00:00\"},"
            "{\"path\":\"C:/tmp/new.pdf\",\"title\":\"新\",\"lastOpened\":\"2026-01-01T00:00:00\"}"
            "]}");
        const HistoryStore store = HistoryStore::fromJson(QJsonDocument::fromJson(raw).object());
        QCOMPARE(store.entries().size(), std::size_t{2});
        QCOMPARE(store.entries()[0].title, QStringLiteral("新"));
    }

    void removeAndClear() {
        HistoryStore store;
        store.recordOpen(path("a.pdf"), QStringLiteral("A"), 1);
        store.recordOpen(path("b.pdf"), QStringLiteral("B"), 1);
        QVERIFY(store.remove(path("a.pdf")));
        QVERIFY(!store.remove(path("a.pdf")));
        QCOMPARE(store.entries().size(), std::size_t{1});
        store.clear();
        QVERIFY(store.entries().empty());
    }

private:
    QString path(const QString& name) const { return dir_->filePath(name); }

    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestHistoryStore)
#include "test_history_store.moc"
