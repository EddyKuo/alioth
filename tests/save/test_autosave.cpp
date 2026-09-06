// 自動儲存與異常復原測試（WBS 5.3、PRD-IO-003）。
//
// 「異常中止」在單元測試裡無法真的製造，但它留下的痕跡可以：自動儲存檔加側錄檔
// 還在，就等於上次沒有走過正常關檔流程。因此這裡驗的是那組痕跡的產生、偵測與清除。

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "engine/save/autosave.h"
#include "engine/save/incremental_saver.h"
#include "engine/save/save_testing.h"
#include "reopen_probe.h"
#include "save_fixture.h"

using namespace alioth::engine::save;
using alioth::test::makeBulkyPdf;
using alioth::test::readAll;
using alioth::test::writePdfTo;

namespace {

constexpr int kPageCount = 3;

}  // namespace

class TestAutosave : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        autosaveDir_ = dir_->filePath(QStringLiteral("autosave"));
        QVERIFY(QDir().mkpath(autosaveDir_));

        original_ = makeBulkyPdf(kPageCount);
        path_ = dir_->filePath(QStringLiteral("doc.pdf"));
        QVERIFY(writePdfTo(path_, original_));
    }

    void cleanup() { dir_.reset(); }

    void defaultIntervalIsTwoMinutes() {
        const AutosaveManager manager(autosaveDir_.toStdString());
        QCOMPARE(manager.interval(), std::chrono::seconds{120});
    }

    // 沒有未存檔變更就不該寫檔；有變更則在間隔到期後才寫。
    void becomesDueOnlyAfterIntervalWithPendingChanges() {
        AutosaveManager manager(autosaveDir_.toStdString());
        manager.setInterval(std::chrono::seconds{120});

        const auto t0 = AutosaveClock::now();
        QVERIFY(!manager.isDue(t0 + std::chrono::hours{1}));

        manager.markDirty(t0);
        QVERIFY(!manager.isDue(t0 + std::chrono::seconds{119}));
        QVERIFY(manager.isDue(t0 + std::chrono::seconds{120}));

        manager.markClean(t0 + std::chrono::seconds{120});
        QVERIFY(!manager.isDue(t0 + std::chrono::hours{1}));
    }

    // 最重要的一條：自動儲存的產物不是原檔。
    void autosaveNeverWritesToTheOriginal() {
        AutosaveManager manager(autosaveDir_.toStdString());
        AutosaveResult result;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            manager.markDirty();
            result = manager.autosave(doc.handle(), path_.toStdString());
        }
        QVERIFY2(result.ok(), describe(result.status));

        const QString autosavePath = QString::fromStdString(result.entry.autosavePath);
        QVERIFY(autosavePath != path_);
        QVERIFY(autosavePath.startsWith(autosaveDir_));
        QVERIFY(QFile::exists(autosavePath));

        // 原檔位元組完全未變。
        QCOMPARE(readAll(path_), original_);
        // 自動儲存完成後不再是 dirty，下一輪不會空轉寫檔。
        QVERIFY(!manager.isDirty());
    }

    // 自動儲存檔本身要是能用的 PDF，且保留原檔前綴——復原不該是降級。
    void autosaveFileIsRecoverableAndKeepsOriginalBytes() {
        AutosaveManager manager(autosaveDir_.toStdString());
        std::string autosavePath;
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            manager.markDirty();
            const AutosaveResult result = manager.autosave(doc.handle(), path_.toStdString());
            QVERIFY(result.ok());
            autosavePath = result.entry.autosavePath;
        }
        const QByteArray saved = readAll(QString::fromStdString(autosavePath));
        QCOMPARE(saved.left(original_.size()), original_);

        const auto pages = alioth::test::probePageCount(autosavePath);
        QVERIFY2(pages.has_value(), "自動儲存檔無法開啟，等於沒有復原能力");
        QCOMPARE(*pages, kPageCount);
    }

    // 側錄檔留著 = 上次異常中止；掃描得到才有復原對話框可言。
    void crashResidueIsDetectable() {
        AutosaveManager manager(autosaveDir_.toStdString());
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            manager.markDirty();
            QVERIFY(manager.autosave(doc.handle(), path_.toStdString()).ok());
        }
        // 此處刻意不呼叫 clearSession()，模擬行程被強制中止。

        const auto entries = AutosaveManager::findRecoverable(autosaveDir_.toStdString());
        QCOMPARE(entries.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(entries[0].originalPath),
                 QDir::cleanPath(QFileInfo(path_).absoluteFilePath()));
        QVERIFY(entries[0].savedAtEpochMs > 0);
        QCOMPARE(entries[0].originalSizeBytes, static_cast<std::uint64_t>(original_.size()));
        QVERIFY(QFile::exists(QString::fromStdString(entries[0].autosavePath)));
    }

    // 正常關檔會清掉痕跡，下次啟動不該再問要不要復原。
    void clearSessionRemovesResidue() {
        AutosaveManager manager(autosaveDir_.toStdString());
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            QVERIFY(support::rotatePage(doc.handle(), 0, 1));
            manager.markDirty();
            QVERIFY(manager.autosave(doc.handle(), path_.toStdString()).ok());
        }
        QVERIFY(manager.clearSession(path_.toStdString()));
        QVERIFY(AutosaveManager::findRecoverable(autosaveDir_.toStdString()).empty());
        QCOMPARE(readAll(path_), original_);
    }

    void discardRemovesBothArtifacts() {
        AutosaveManager manager(autosaveDir_.toStdString());
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            manager.markDirty();
            QVERIFY(manager.autosave(doc.handle(), path_.toStdString()).ok());
        }
        auto entries = AutosaveManager::findRecoverable(autosaveDir_.toStdString());
        QCOMPARE(entries.size(), std::size_t{1});
        QVERIFY(AutosaveManager::discard(entries[0]));
        QVERIFY(!QFile::exists(QString::fromStdString(entries[0].autosavePath)));
        QVERIFY(!QFile::exists(QString::fromStdString(entries[0].sidecarPath)));
        QVERIFY(AutosaveManager::findRecoverable(autosaveDir_.toStdString()).empty());
    }

    // 反覆自動儲存同一份文件只留一個檔案，不在快取目錄裡堆副本。
    void repeatedAutosaveReusesTheSameFile() {
        AutosaveManager manager(autosaveDir_.toStdString());
        {
            ScopedDocument doc;
            QVERIFY(doc.open(path_.toStdString()));
            for (int i = 0; i < 3; ++i) {
                QVERIFY(support::rotatePage(doc.handle(), 0, 1));
                manager.markDirty();
                QVERIFY(manager.autosave(doc.handle(), path_.toStdString()).ok());
            }
        }
        const QStringList files = QDir(autosaveDir_).entryList(QDir::Files);
        QCOMPARE(files.size(), 2);  // 一個 PDF、一個側錄檔
    }

    // 不同目錄下的同名檔不得互相覆蓋。
    void distinctDocumentsGetDistinctSlots() {
        const QString otherDir = dir_->filePath(QStringLiteral("nested"));
        const QString other = otherDir + QStringLiteral("/doc.pdf");
        QVERIFY(writePdfTo(other, original_));

        const std::string a = AutosaveManager::autosavePathFor(path_.toStdString(),
                                                               autosaveDir_.toStdString());
        const std::string b = AutosaveManager::autosavePathFor(other.toStdString(),
                                                               autosaveDir_.toStdString());
        QVERIFY(a != b);
    }

    void missingSourceIsReportedNotSilentlySkipped() {
        AutosaveManager manager(autosaveDir_.toStdString());
        ScopedDocument doc;
        QVERIFY(doc.open(path_.toStdString()));
        const AutosaveResult result =
            manager.autosave(doc.handle(), dir_->filePath(QStringLiteral("gone.pdf")).toStdString());
        QCOMPARE(result.status, SaveStatus::SourceUnreadable);
        QVERIFY(!result.message.empty());
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString autosaveDir_;
    QByteArray original_;
    QString path_;
};

QTEST_APPLESS_MAIN(TestAutosave)
#include "test_autosave.moc"
