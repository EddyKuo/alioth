// 外部變更偵測測試（WP27，PRD-IO-004）。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include "platform/external_change.h"

using namespace alioth::platform;

class TestExternalChange : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("watched.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(8192, 'A'));
        file.close();
    }

    void cleanup() { dir_.reset(); }

    void unchangedFileReportsUnchanged() {
        const FileSnapshot snapshot = captureSnapshot(path_);
        QVERIFY(snapshot.valid());
        QCOMPARE(checkForExternalChange(path_, snapshot), ExternalChangeStatus::Unchanged);
    }

    void contentChangeWithSameSizeIsDetected() {
        const FileSnapshot snapshot = captureSnapshot(path_);
        QVERIFY(snapshot.valid());

        // 時間戳解析度在部分檔案系統上是秒級，測試不能依賴「時間一定會往前走」；
        // 用尾端內容改變（大小不變）驗證雜湊比對本身有沒有生效。
        QThread::msleep(1100);
        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(file.size() - 4));
        file.write("ZZZZ");
        file.close();

        QCOMPARE(checkForExternalChange(path_, snapshot), ExternalChangeStatus::ModifiedExternally);
    }

    void sizeChangeIsDetected() {
        const FileSnapshot snapshot = captureSnapshot(path_);
        QVERIFY(snapshot.valid());

        QFile file(path_);
        QVERIFY(file.open(QIODevice::Append));
        file.write("more bytes appended");
        file.close();

        QCOMPARE(checkForExternalChange(path_, snapshot), ExternalChangeStatus::ModifiedExternally);
    }

    void deletionIsDetected() {
        const FileSnapshot snapshot = captureSnapshot(path_);
        QVERIFY(snapshot.valid());

        QVERIFY(QFile::remove(path_));

        QCOMPARE(checkForExternalChange(path_, snapshot), ExternalChangeStatus::DeletedExternally);
    }

    void missingSourceSnapshotIsUnknown() {
        const FileSnapshot neverOpened;  // exists 預設為 false
        QVERIFY(!neverOpened.valid());
        QCOMPARE(checkForExternalChange(path_, neverOpened), ExternalChangeStatus::Unknown);
    }

    void snapshotOfMissingFileIsInvalid() {
        const FileSnapshot snapshot = captureSnapshot(dir_->filePath(QStringLiteral("nope.pdf")));
        QVERIFY(!snapshot.valid());
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_APPLESS_MAIN(TestExternalChange)
#include "test_external_change.moc"
