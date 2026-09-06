// 增量儲存器測試（WBS 5.1、PRD-IO-001、PRD §8.3、§8.4）。
//
// 這組測試的核心命題只有一句：增量儲存後，原檔的位元組必須一個都沒有變。
// 那正是數位簽章 /ByteRange 涵蓋的範圍——前綴一旦被改寫，簽章就從
// 「有效，簽章後有變更」變成「無效」，而那是本產品賣點的直接失效。

#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "engine/save/incremental_saver.h"
#include "engine/save/save_testing.h"
#include "platform/atomic_file.h"
#include "reopen_probe.h"
#include "save_fixture.h"

using namespace alioth::engine::save;
using alioth::test::makeBulkyPdf;
using alioth::test::readAll;
using alioth::test::writePdfTo;

namespace {

constexpr int kPageCount = 4;

}  // namespace

class TestIncrementalSaver : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        original_ = makeBulkyPdf(kPageCount);
        path_ = dir_->filePath(QStringLiteral("source.pdf"));
        QVERIFY(writePdfTo(path_, original_));
    }

    void cleanup() { dir_.reset(); }

    // 增量儲存的定義本身：原內容只能被追加，不能被重寫。
    void incrementalSaveOnlyAppends() {
        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            result = IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                       path_.toStdString());
        }
        QVERIFY2(result.ok(), describe(result.status));
        QVERIFY2(!result.fullRewriteFallback, "PDFium 退回整份重寫，簽章保全已失效");

        const QByteArray saved = readAll(path_);
        QVERIFY(saved.size() > original_.size());
        QCOMPARE(saved.left(original_.size()), original_);
    }

    // 增量段的量級要與原檔差一個數量級以上，否則「增量」只是名義上的
    // （PRD-IO-001 的驗收數字是 100 MB 文件 ≤ 20 KB）。
    void incrementalDeltaIsMuchSmallerThanSource() {
        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            result = IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                       path_.toStdString());
        }
        QVERIFY(result.ok());
        QCOMPARE(result.metrics.sourceBytes, static_cast<std::uint64_t>(original_.size()));
        QVERIFY(result.metrics.incrementalBytes > 0);
        QVERIFY2(result.metrics.incrementalBytes * 20 < result.metrics.sourceBytes,
                 qPrintable(QStringLiteral("增量 %1 位元組 / 原檔 %2 位元組")
                                .arg(result.metrics.incrementalBytes)
                                .arg(result.metrics.sourceBytes)));
        QCOMPARE(result.metrics.totalBytes,
                 result.metrics.sourceBytes + result.metrics.incrementalBytes);
        // PRD-IO-001 的絕對上限：一筆變更追加的位元組不得超過 20 KB。
        QVERIFY(result.metrics.incrementalBytes <= 20u * 1024u);
    }

    // 存檔結果必須還是 PDFium 開得起來的文件，頁數不變。
    void savedDocumentRemainsReadable() {
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            QVERIFY(IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                      path_.toStdString())
                        .ok());
        }
        const auto pages = alioth::test::probePageCount(path_.toStdString());
        QVERIFY2(pages.has_value(), "增量儲存後的檔案無法重新開啟");
        QCOMPARE(*pages, kPageCount);
    }

    // 對照組：另存新檔重寫整份，前綴必然改變——這正是它會讓既有簽章失效的原因。
    void saveAsCopyRewritesWholeFile() {
        const QString copyPath = dir_->filePath(QStringLiteral("copy.pdf"));
        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            result = IncrementalSaver::saveAsCopy(doc.handle(), copyPath.toStdString());
        }
        QVERIFY2(result.ok(), describe(result.status));

        const QByteArray copy = readAll(copyPath);
        QVERIFY(!copy.isEmpty());
        QVERIFY2(copy.left(original_.size()) != original_,
                 "另存新檔竟保留了原檔前綴，代表它其實沒有重寫");
        QVERIFY(copy.startsWith("%PDF-"));

        const auto pages = alioth::test::probePageCount(copyPath.toStdString());
        QVERIFY(pages.has_value());
        QCOMPARE(*pages, kPageCount);

        // 原檔不得被另存動作波及。
        QCOMPARE(readAll(path_), original_);
    }

    // PRD §8.3：寫出保留原版本號，不降級。要求 1.4 也不會讓 1.7 的檔案退回去。
    void fileVersionIsNeverDowngraded() {
        SaveOptions options;
        options.fileVersion = 14;
        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QCOMPARE(doc.fileVersion(), 17);
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            result = IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                       path_.toStdString(), options);
        }
        QVERIFY(result.ok());
        QCOMPARE(result.fileVersion, 17);
        QVERIFY(readAll(path_).startsWith("%PDF-1.7"));
    }

    // 存檔失敗時原檔必須毫髮無傷——半毀的原檔比存檔失敗嚴重得多。
    void failedSaveLeavesOriginalIntact() {
        const SaveResult result =
            IncrementalSaver::saveIncremental(nullptr, path_.toStdString(), path_.toStdString());
        QVERIFY(!result.ok());
        QCOMPARE(result.status, SaveStatus::InvalidDocument);
        QCOMPARE(readAll(path_), original_);

        const auto pages = alioth::test::probePageCount(path_.toStdString());
        QVERIFY(pages.has_value());
        QCOMPARE(*pages, kPageCount);
    }

    // 原子寫入的中途放棄：暫存檔被清掉，目標檔停留在舊版本。
    // 這就是斷電情境在使用者空間的可測近似。
    void abortedAtomicWriteDoesNotTouchTarget() {
        {
            alioth::platform::AtomicFileWriter writer(path_);
            QVERIFY(writer.begin());
            QVERIFY(writer.copyFrom(path_));
            const QByteArray garbage(4096, '\x01');
            QVERIFY(writer.write(garbage.constData(), static_cast<std::size_t>(garbage.size())));
            // 刻意不 commit，解構時應自行清理。
        }
        QCOMPARE(readAll(path_), original_);

        const auto pages = alioth::test::probePageCount(path_.toStdString());
        QVERIFY2(pages.has_value(), "放棄的寫入把原檔弄壞了");
        QCOMPARE(*pages, kPageCount);

        // 暫存檔不得殘留在使用者的文件目錄裡。
        const QStringList leftovers =
            QDir(dir_->path()).entryList(QStringList{QStringLiteral("*.tmp")},
                                         QDir::Files | QDir::Hidden);
        QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(','))));
    }

    // 唯讀目標要在動工前就被擋下（PRD-IO-005：唯讀時應改走另存）。
    void readOnlyTargetIsRejectedUpFront() {
        const QString target = dir_->filePath(QStringLiteral("readonly.pdf"));
        QVERIFY(writePdfTo(target, original_));
        QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::ReadUser));

        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            result = IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                       target.toStdString());
        }
        QCOMPARE(result.status, SaveStatus::TargetNotWritable);
        QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ReadUser | QFileDevice::WriteUser));
        QCOMPARE(readAll(target), original_);
    }

    // 效能預算要能被量測到，否則 PRD-IO-001 的 300 毫秒只是願望。
    void metricsAreObservable() {
        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            result = IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                       path_.toStdString());
        }
        QVERIFY(result.ok());
        QVERIFY(result.metrics.totalMs > 0.0);
        QVERIFY(result.metrics.pdfiumMs > 0.0);
        QVERIFY(result.metrics.totalMs >= result.metrics.pdfiumMs);
        qInfo("增量儲存：總計 %.2f ms（PDFium+寫入 %.2f / 同步 %.2f / 更名 %.2f），"
              "原檔 %llu 位元組、追加 %llu 位元組",
              result.metrics.totalMs, result.metrics.pdfiumMs, result.metrics.syncMs,
              result.metrics.renameMs,
              static_cast<unsigned long long>(result.metrics.sourceBytes),
              static_cast<unsigned long long>(result.metrics.incrementalBytes));
    }

    // 增量儲存到另一個路徑時，來源檔不得被動到。
    void incrementalSaveToOtherPathKeepsSource() {
        const QString target = dir_->filePath(QStringLiteral("derived.pdf"));
        SaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            result = IncrementalSaver::saveIncremental(doc.handle(), path_.toStdString(),
                                                       target.toStdString());
        }
        QVERIFY(result.ok());
        QCOMPARE(readAll(path_), original_);
        QCOMPARE(readAll(target).left(original_.size()), original_);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QByteArray original_;
    QString path_;
};

QTEST_APPLESS_MAIN(TestIncrementalSaver)
#include "test_incremental_saver.moc"
