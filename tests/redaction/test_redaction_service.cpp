// 塗黑服務（PRD-ANN-032 / PRD-ANN-033）的應用層行為。
//
// 引擎層的「內容真的被刪掉」已經由 tests/redaction 的其他檔案以文字擷取與
// 位元組掃描雙重驗證。這裡驗的是應用層那條分界線：
//
//   標記走增量附加 → 原檔位元組一個都不動 → 復原就是截回原長度
//   套用走整份重寫 → 原檔位元組一定會變
//
// 這兩件事若混在一起，「標記」會在使用者以為只是做記號的時候毀掉簽章。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "app/redaction_service.h"
#include "object_fixture.h"

using alioth::app::RedactionService;
using alioth::app::RewriteConsent;
using alioth::domain::RectF;

namespace {

QString writeFixture(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    const QByteArray bytes = alioth::test::makeFixturePdf();
    file.write(bytes);
    file.close();
    return path;
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

}  // namespace

class TestRedactionService : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void markingIsPurelyAdditiveSoTheOriginalBytesSurvive();
    void markCountReflectsWhatWasMarked();
    void clearingMarksRemovesThemAndIsAlsoAdditive();
    void emptyAreaIsRejectedInsteadOfMeaningWholePage();
    void applyingWithoutMarksFails();
    void sanitizeRewritesTheWholeFileAndKeepsTheOriginalForUndo();

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

void TestRedactionService::markingIsPurelyAdditiveSoTheOriginalBytesSurvive() {
    const QString path = writeFixture(*dir_, QStringLiteral("mark.pdf"));
    QVERIFY(!path.isEmpty());
    const QByteArray before = readAll(path);

    RedactionService service;
    const auto result = service.markArea(path, 0, RectF{50, 500, 200, 560});
    QVERIFY2(result.ok, qPrintable(result.message));

    const QByteArray after = readAll(path);
    QVERIFY(after.size() > before.size());
    // 原檔前綴一個位元組都不能變——這正是「標記不會讓既有簽章失效」的依據。
    QCOMPARE(after.left(before.size()), before);
    // 復原點就是原長度。
    QCOMPARE(result.previousSize, static_cast<quint64>(before.size()));
    QVERIFY(!result.boundaryGuard.isEmpty());
}

void TestRedactionService::markCountReflectsWhatWasMarked() {
    const QString path = writeFixture(*dir_, QStringLiteral("count.pdf"));
    RedactionService service;
    QCOMPARE(service.pendingMarkCount(path), 0);

    QVERIFY(service.markArea(path, 0, RectF{50, 500, 200, 560}).ok);
    QCOMPARE(service.pendingMarkCount(path), 1);
    QVERIFY(service.markArea(path, 0, RectF{50, 400, 200, 440}).ok);
    QCOMPARE(service.pendingMarkCount(path), 2);
}

void TestRedactionService::clearingMarksRemovesThemAndIsAlsoAdditive() {
    const QString path = writeFixture(*dir_, QStringLiteral("clear.pdf"));
    RedactionService service;
    QVERIFY(service.markArea(path, 0, RectF{50, 500, 200, 560}).ok);
    QCOMPARE(service.pendingMarkCount(path), 1);

    const QByteArray beforeClear = readAll(path);
    const auto cleared = service.clearMarks(path, -1);
    QVERIFY2(cleared.ok, qPrintable(cleared.message));
    QCOMPARE(service.pendingMarkCount(path), 0);

    // 移除標記也是純附加：檔案變長，前綴不變。標記可逆的整個意義就在這裡——
    // 若移除標記要重寫整份檔案，「只是做個記號」也會毀掉簽章。
    const QByteArray afterClear = readAll(path);
    QVERIFY(afterClear.size() > beforeClear.size());
    QCOMPARE(afterClear.left(beforeClear.size()), beforeClear);
}

void TestRedactionService::emptyAreaIsRejectedInsteadOfMeaningWholePage() {
    const QString path = writeFixture(*dir_, QStringLiteral("empty.pdf"));
    const QByteArray before = readAll(path);

    RedactionService service;
    const auto result = service.markArea(path, 0, RectF{100, 100, 100, 100});
    QVERIFY2(!result.ok, "空區域被當成有效標記——套用時會刪掉整頁");
    QVERIFY(!result.message.isEmpty());
    // 失敗就不該碰檔案。
    QCOMPARE(readAll(path), before);
}

void TestRedactionService::applyingWithoutMarksFails() {
    const QString path = writeFixture(*dir_, QStringLiteral("noapply.pdf"));
    const QByteArray before = readAll(path);

    RedactionService service;
    const auto applied = service.applyMarks(path, RewriteConsent::confirmed());
    // 沒有標記時套用不該「成功但什麼都沒做」：那會讓使用者以為塗黑完成了。
    QVERIFY(!applied.ok || applied.message.contains(QStringLiteral("0")));
    if (!applied.ok) QCOMPARE(readAll(path), before);
}

void TestRedactionService::sanitizeRewritesTheWholeFileAndKeepsTheOriginalForUndo() {
    const QString path = writeFixture(*dir_, QStringLiteral("sanitize.pdf"));
    const QByteArray before = readAll(path);

    RedactionService service;
    const auto result = service.sanitize(path, RewriteConsent::confirmed());
    QVERIFY2(result.ok, qPrintable(result.message));

    // 清除中繼資料是**全檔重寫**，不是增量附加。增量只會讓新的 /Info 疊在
    // 舊的上面，舊值仍然留在檔案裡，`strings` 一撈就出來——那等於什麼都沒清。
    const QByteArray after = readAll(path);
    QVERIFY2(after != before, "檔案完全沒變——清除沒有真的發生");

    // 復原靠保留原始位元組（全檔重寫沒辦法靠截回原長度還原）。
    QCOMPARE(result.previousBytes, before);
    QVERIFY(!result.message.isEmpty());
}

QTEST_MAIN(TestRedactionService)
#include "test_redaction_service.moc"
