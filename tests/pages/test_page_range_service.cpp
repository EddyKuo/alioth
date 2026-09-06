// 刪除與擷取頁面的應用層行為（PRD-PAGE-002）。
//
// 引擎的 PageEditor 已經有自己的測試。這裡驗的是應用層加上去的兩道判斷，
// 而它們都是「不做會出人命」的那種：
//
//   - 刪光所有頁面會產生一份多數檢視器打不開的 PDF，使用者拿到的是一個
//     壞掉的檔案，而不是一個空文件
//   - 擷取的輸出蓋掉來源，等於一次不可復原的刪頁，而使用者以為自己
//     只是「另存一份」

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "app/page_operations_service.h"
#include "engine/pages/page_editor.h"

using alioth::app::PageOperationsService;
using alioth::app::RewriteConsent;

namespace {

// 造一份三頁的文件。用 PageEditor 自己造，不依賴任何外部語料。
QString makeThreePagePdf(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    alioth::engine::pages::PageEditor editor;
    editor.createEmpty();
    if (!editor.insertBlankPages(0, 3, 595.0, 842.0).ok()) return {};
    if (!editor.saveAsCopy(path.toStdString(), alioth::engine::save::SaveOptions{}).ok()) {
        return {};
    }
    return path;
}

int pageCountOf(const QString& path) {
    alioth::engine::pages::PageEditor editor;
    if (!editor.open(path.toStdString(), "")) return -1;
    return editor.pageCount();
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

}  // namespace

class TestPageRangeService : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void deletingKeepsTheRestAndPreservesOriginalForUndo();
    void deletingEveryPageIsRefused();
    void extractingLeavesTheSourceUntouched();
    void extractingOntoTheSourceIsRefused();
    void mergingRefusesToWriteOverASource();
    void mergingProducesTheSumOfThePages();
    void splittingProducesOneFilePerChunk();

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

void TestPageRangeService::deletingKeepsTheRestAndPreservesOriginalForUndo() {
    const QString path = makeThreePagePdf(*dir_, QStringLiteral("del.pdf"));
    QVERIFY(!path.isEmpty());
    const QByteArray before = readAll(path);
    QCOMPARE(pageCountOf(path), 3);

    PageOperationsService service;
    const auto result = service.deletePages(path, {1}, RewriteConsent::confirmed());
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(pageCountOf(path), 2);
    // 全檔重寫沒辦法靠截回原長度還原，所以原始位元組必須留著。
    QCOMPARE(result.previousBytes, before);
}

void TestPageRangeService::deletingEveryPageIsRefused() {
    const QString path = makeThreePagePdf(*dir_, QStringLiteral("all.pdf"));
    const QByteArray before = readAll(path);

    PageOperationsService service;
    const auto result = service.deletePages(path, {0, 1, 2}, RewriteConsent::confirmed());
    QVERIFY2(!result.ok, "刪光所有頁面被允許了——產出的檔案多數檢視器打不開");
    QCOMPARE(readAll(path), before);
}

void TestPageRangeService::extractingLeavesTheSourceUntouched() {
    const QString path = makeThreePagePdf(*dir_, QStringLiteral("src.pdf"));
    const QByteArray before = readAll(path);
    const QString target = dir_->filePath(QStringLiteral("out.pdf"));

    PageOperationsService service;
    const auto result = service.extractPages(path, {0, 2}, target);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.pageCount, 2);
    QCOMPARE(pageCountOf(target), 2);
    // 原檔一個位元組都不能變——擷取是「另存一份」，不是編輯。
    QCOMPARE(readAll(path), before);
    // 沒有東西要復原，所以不該留下原始位元組讓命令堆疊以為可以回退。
    QVERIFY(result.previousBytes.isEmpty());
}

void TestPageRangeService::extractingOntoTheSourceIsRefused() {
    const QString path = makeThreePagePdf(*dir_, QStringLiteral("self.pdf"));
    const QByteArray before = readAll(path);

    PageOperationsService service;
    const auto result = service.extractPages(path, {0}, path);
    QVERIFY2(!result.ok, "擷取覆蓋了來源——那等於一次不可復原的刪頁");
    QCOMPARE(readAll(path), before);
}

void TestPageRangeService::mergingRefusesToWriteOverASource() {
    const QString a = makeThreePagePdf(*dir_, QStringLiteral("a.pdf"));
    const QString b = makeThreePagePdf(*dir_, QStringLiteral("b.pdf"));
    const QByteArray before = readAll(a);

    PageOperationsService service;
    // 輸出蓋掉其中一份來源，等於在讀取途中把它換掉——結果不可預期。
    const auto result = service.mergeDocuments({a, b}, a);
    QVERIFY2(!result.ok, "合併覆蓋了來源檔");
    QCOMPARE(readAll(a), before);
}

void TestPageRangeService::mergingProducesTheSumOfThePages() {
    const QString a = makeThreePagePdf(*dir_, QStringLiteral("m1.pdf"));
    const QString b = makeThreePagePdf(*dir_, QStringLiteral("m2.pdf"));
    const QString target = dir_->filePath(QStringLiteral("merged.pdf"));
    const QByteArray beforeA = readAll(a);

    PageOperationsService service;
    const auto result = service.mergeDocuments({a, b}, target);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(pageCountOf(target), 6);
    // 來源一個位元組都不能變——合併是「另存一份」，不是編輯。
    QCOMPARE(readAll(a), beforeA);
    QVERIFY(result.previousBytes.isEmpty());
}

void TestPageRangeService::splittingProducesOneFilePerChunk() {
    const QString path = makeThreePagePdf(*dir_, QStringLiteral("split.pdf"));
    const QByteArray before = readAll(path);
    const QString pattern = dir_->filePath(QStringLiteral("part-{n}.pdf"));

    PageOperationsService service;
    const auto result = service.splitDocument(path, 1, pattern);
    QVERIFY2(result.ok, qPrintable(result.message));
    // 三頁、每份一頁 → 三個檔案。
    QCOMPARE(result.pageCount, 3);
    QCOMPARE(pageCountOf(dir_->filePath(QStringLiteral("part-1.pdf"))), 1);
    QCOMPARE(pageCountOf(dir_->filePath(QStringLiteral("part-3.pdf"))), 1);
    QCOMPARE(readAll(path), before);
}

QTEST_MAIN(TestPageRangeService)
#include "test_page_range_service.moc"
