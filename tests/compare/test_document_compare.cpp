// 文件比對端到端（PRD-CMP-001，M 優先級、R1 基礎版）。
//
// 前面兩支測試分別驗了 diff 演算法與頁面對齊，但那兩層都不碰 PDFium。
// 這一支把整條線接起來：真的兩份 PDF 進去，差異出來。
// 兩層各自正確、接起來卻不對，是這種分層設計最典型的失敗形態。

#include <QtTest>

#include <QTemporaryDir>

#include <string>
#include <vector>

#include "compare_pdf_fixture.h"
#include "engine/compare/document_comparer.h"

using namespace alioth;
using namespace alioth::engine::compare;

namespace {

using alioth::test::ComparePage;

ComparePage page(const std::vector<std::string>& lines) {
    ComparePage p;
    p.lines = lines;
    return p;
}

}  // namespace

class TestDocumentCompare : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void identicalDocumentsHaveNoChanges() {
        const std::vector<ComparePage> pages{page({"first line here", "second line here"}),
                                             page({"third line here"})};
        const QString a = write("a.pdf", pages);
        const QString b = write("b.pdf", pages);

        const CompareResult result = compareDocuments(a.toStdString(), b.toStdString());
        QVERIFY2(result.ok(), "比對失敗");

        for (const domain::PageDiffSummary& summary : result.diff.summaries()) {
            QVERIFY2(!summary.changed(), "完全相同的兩份文件被報出差異");
        }
    }

    void singleWordChangeIsLocalised() {
        // 改一個詞不該把整頁報成變更——那等於沒有比對。
        const QString a = write("a.pdf", {page({"the quick brown fox jumps over the lazy dog"})});
        const QString b = write("b.pdf", {page({"the quick brown cat jumps over the lazy dog"})});

        const CompareResult result = compareDocuments(a.toStdString(), b.toStdString());
        QVERIFY(result.ok());
        QCOMPARE(result.diff.summaries().size(), std::size_t{1});

        const domain::PageDiffSummary& summary = result.diff.summaries().front();
        QCOMPARE(summary.kind, domain::PageMatchKind::Matched);
        QVERIFY(summary.changed());
        // 一個詞換成另一個詞：變更數應該很小，而不是整頁的詞數。
        QVERIFY2(summary.totalChanges() <= 3,
                 qPrintable(QStringLiteral("只改一個詞卻報出 %1 處變更")
                                .arg(summary.totalChanges())));
    }

    void insertedPageDoesNotCascade() {
        // 與 test_page_align 的同名情境對應，但這裡走的是真的 PDF。
        const QString a = write("a.pdf", {page({"alpha content here"}),
                                          page({"beta content here"}),
                                          page({"gamma content here"})});
        const QString b = write("b.pdf", {page({"alpha content here"}),
                                          page({"brand new page inserted"}),
                                          page({"beta content here"}),
                                          page({"gamma content here"})});

        const CompareResult result = compareDocuments(a.toStdString(), b.toStdString());
        QVERIFY(result.ok());

        int inserted = 0;
        int changedMatches = 0;
        for (const domain::PageDiffSummary& summary : result.diff.summaries()) {
            if (summary.kind == domain::PageMatchKind::Inserted) ++inserted;
            if (summary.kind == domain::PageMatchKind::Matched && summary.changed()) {
                ++changedMatches;
            }
        }
        QCOMPARE(inserted, 1);
        QVERIFY2(changedMatches == 0,
                 "插入一頁之後，其他頁面被誤報為有變更（對齊沒有吸收位移）");
    }

    void deletedPageIsReported() {
        const QString a = write("a.pdf", {page({"alpha content here"}),
                                          page({"beta content here"})});
        const QString b = write("b.pdf", {page({"alpha content here"})});

        const CompareResult result = compareDocuments(a.toStdString(), b.toStdString());
        QVERIFY(result.ok());

        int deleted = 0;
        for (const domain::PageDiffSummary& summary : result.diff.summaries()) {
            if (summary.kind == domain::PageMatchKind::Deleted) ++deleted;
        }
        QCOMPARE(deleted, 1);
    }

    void caseSensitivityIsConfigurable() {
        // 預設「Alioth」改成「ALIOTH」算一次修改：審閱情境下那確實是改動。
        const QString a = write("a.pdf", {page({"Alioth review station"})});
        const QString b = write("b.pdf", {page({"ALIOTH review station"})});

        const CompareResult sensitive = compareDocuments(a.toStdString(), b.toStdString());
        QVERIFY(sensitive.ok());
        QVERIFY(sensitive.diff.summaries().front().changed());

        CompareOptions options;
        options.ignoreCase = true;
        const CompareResult insensitive =
            compareDocuments(a.toStdString(), b.toStdString(), options);
        QVERIFY(insensitive.ok());
        QVERIFY2(!insensitive.diff.summaries().front().changed(),
                 "設了 ignoreCase 卻仍把大小寫差異報成變更");
    }

    void missingFileNamesWhichSideFailed() {
        // 「開檔失敗」而不說是哪一份，使用者得自己猜。
        const QString a = write("a.pdf", {page({"content"})});

        const CompareResult result =
            compareDocuments(a.toStdString(), "no-such-file.pdf");
        QVERIFY(!result.ok());
        QVERIFY2(!result.oldDocumentFailed, "錯的是新文件，卻回報成舊文件");

        const CompareResult reversed =
            compareDocuments("no-such-file.pdf", a.toStdString());
        QVERIFY(!reversed.ok());
        QVERIFY(reversed.oldDocumentFailed);
    }

    void duplicatePagesAreFoundInARealDocument() {
        const QString path = write("dup.pdf", {page({"repeated content here"}),
                                               page({"unique content here"}),
                                               page({"repeated content here"})});

        const DuplicateScanResult result = scanDuplicatePages(path.toStdString());
        QVERIFY(result.ok());
        QCOMPARE(result.groups.size(), std::size_t{1});
        QCOMPARE(result.groups.front().count(), std::size_t{2});
    }

private:
    QString write(const QString& name, const std::vector<ComparePage>& pages) {
        const QString path = dir_->filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        const QByteArray bytes = test::makeComparePdf(pages);
        file.write(bytes);
        file.close();
        return path;
    }

    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestDocumentCompare)
#include "test_document_compare.moc"
