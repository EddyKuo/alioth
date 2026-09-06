// 匯出純文字的測試（WP27，PRD-IO-007 後半）。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <condition_variable>
#include <mutex>

#include "engine/iosec/text_export.h"
#include "text/text_pdf_fixture.h"

using namespace alioth::engine::iosec;
using alioth::engine::text::TextExtractor;

namespace {

template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }
    bool wait(int milliseconds = 20000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }
    const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

}  // namespace

class TestTextExport : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("source.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(alioth::test::makeTextPdf());
        file.close();
    }

    void cleanup() { dir_.reset(); }

    void exportsAllPagesWithPageBreak() {
        TextExtractor extractor;
        Latch<alioth::domain::DocumentError> openLatch;
        extractor.open(path_.toStdString(), {},
                       [&openLatch](alioth::domain::DocumentError error) {
                           openLatch.set(error);
                       });
        QVERIFY(openLatch.wait());
        QCOMPARE(openLatch.value(), alioth::domain::DocumentError::None);
        QCOMPARE(extractor.pageCount(), 2);

        const QString outputPath = dir_->filePath(QStringLiteral("out.txt"));
        Latch<TextExportResult> exportLatch;
        exportDocumentText(extractor, outputPath.toStdString(),
                           [&exportLatch](TextExportResult result) {
                               exportLatch.set(std::move(result));
                           });
        QVERIFY(exportLatch.wait());
        const TextExportResult& result = exportLatch.value();
        QVERIFY2(result.ok(), result.message.c_str());
        QCOMPARE(result.pagesExported, 2);

        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        const QByteArray text = output.readAll();
        QVERIFY(text.contains(alioth::test::kLine0));
        QVERIFY(text.contains(alioth::test::kLine2));
        QVERIFY(text.contains(alioth::test::kPage2Line1));
        // 頁與頁之間必須有分隔字元，否則第一頁最後一個字與第二頁第一個字
        // 會直接黏在一起，讀起來像是同一行。
        QVERIFY(text.contains('\f'));

        extractor.close();
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_APPLESS_MAIN(TestTextExport)
#include "test_text_export.moc"
