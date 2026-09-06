// 另存新檔與最佳化的測試（WP27，PRD-IO-002）。
//
// 兩個命題各對應一支測試：
//   一、簽章防呆——文件有簽章時一律拒絕，除非明確確認。這是介面層級的保證，
//      不需要真的簽過章，用一個假的簽章數字就能測到「防呆本身」。
//   二、移除未使用物件不是文件裡寫的承諾，是可以量出來的事實——
//      在原檔裡塞一個沒有任何東西參照的物件，最佳化之後那個物件編號
//      必須從輸出檔裡消失。這條測試如果哪天紅了，代表 PDFium 的整份重寫
//      不再做可達性標記，engine/save/optimize_saver.h 檔頭的假設就要重寫。

#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/save/incremental_saver.h"
#include "engine/save/optimize_saver.h"
#include "qa/qpdf_check.h"
#include "save/save_fixture.h"

using namespace alioth::engine::save;
using namespace alioth::engine::objects;
using alioth::test::makeBulkyPdf;
using alioth::test::readAll;
using alioth::test::writePdfTo;

namespace {

// 在原檔尾端附加一個沒有任何物件參照它的字典物件，回傳它的物件編號。
int appendOrphanObject(const QByteArray& sourceBytes, QByteArray* outBytes) {
    IncrementalAppender appender;
    std::string diagnostic;
    const SourceStatus status = appender.open(
        std::string(sourceBytes.constData(), static_cast<std::size_t>(sourceBytes.size())),
        &diagnostic);
    if (status != SourceStatus::Ok) return 0;

    const int orphan = appender.allocateObject();
    PdfDictionary dict;
    dict.set("Type", makeName("AliothTestOrphan"));
    appender.setObject(orphan, PdfObject(std::move(dict)));

    const BuildResult built = appender.build();
    if (!built.ok) return 0;
    *outBytes = QByteArray(built.bytes.data(), static_cast<int>(built.bytes.size()));
    return orphan;
}

}  // namespace

class TestOptimizeSaver : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void cleanup() { dir_.reset(); }

    void refusesWhenSignaturesPresentAndNotAcknowledged() {
        const QByteArray original = makeBulkyPdf(1, 4000);
        const QString path = dir_->filePath(QStringLiteral("signed.pdf"));
        QVERIFY(writePdfTo(path, original));

        ScopedDocument doc;
        QVERIFY(doc.open(path.toStdString()));

        const QString target = dir_->filePath(QStringLiteral("optimized.pdf"));
        const OptimizeResult result =
            optimizeDocument(doc.handle(), target.toStdString(), /*signatureCount=*/1);

        QCOMPARE(result.status, OptimizeStatus::RefusedSignaturePresent);
        QVERIFY(!result.message.empty());
        QVERIFY(!QFileInfo::exists(target));
    }

    void proceedsWhenSignatureLossAcknowledged() {
        const QByteArray original = makeBulkyPdf(1, 4000);
        const QString path = dir_->filePath(QStringLiteral("signed.pdf"));
        QVERIFY(writePdfTo(path, original));

        ScopedDocument doc;
        QVERIFY(doc.open(path.toStdString()));

        const QString target = dir_->filePath(QStringLiteral("optimized.pdf"));
        OptimizeOptions options;
        options.acknowledgeSignatureLoss = true;
        const OptimizeResult result =
            optimizeDocument(doc.handle(), target.toStdString(), /*signatureCount=*/1, options);

        QVERIFY2(result.ok(), result.message.c_str());
        QVERIFY(QFileInfo::exists(target));
    }

    // 要求線性化必須誠實地回報做不到，不能悄悄忽略也不能假裝完成。
    void linearizationRequestIsReportedAsUnsupported() {
        const QByteArray original = makeBulkyPdf(1, 4000);
        const QString path = dir_->filePath(QStringLiteral("plain.pdf"));
        QVERIFY(writePdfTo(path, original));

        ScopedDocument doc;
        QVERIFY(doc.open(path.toStdString()));

        const QString target = dir_->filePath(QStringLiteral("optimized.pdf"));
        OptimizeOptions options;
        options.requestLinearization = true;
        const OptimizeResult result =
            optimizeDocument(doc.handle(), target.toStdString(), /*signatureCount=*/0, options);

        QVERIFY2(result.ok(), result.message.c_str());
        QVERIFY(!result.linearizationApplied);
        QVERIFY2(!result.linearizationNote.empty(),
                 "要求線性化卻沒有解釋為什麼做不到，使用者會以為程式忘了做");
    }

    void removesUnreferencedObjects() {
        const QByteArray original = makeBulkyPdf(1, 4000);
        QByteArray withOrphan;
        const int orphanNumber = appendOrphanObject(original, &withOrphan);
        QVERIFY2(orphanNumber > 0, "測試前置：附加孤兒物件失敗");

        const QString path = dir_->filePath(QStringLiteral("orphan.pdf"));
        QVERIFY(writePdfTo(path, withOrphan));

        // 前置驗證：孤兒物件確實存在於來源檔裡，否則後面「消失了」的斷言毫無意義。
        {
            PdfSourceDocument before;
            QCOMPARE(before.open(std::string(withOrphan.constData(),
                                             static_cast<std::size_t>(withOrphan.size()))),
                    SourceStatus::Ok);
            QVERIFY(before.hasObject(orphanNumber));
        }

        ScopedDocument doc;
        QVERIFY(doc.open(path.toStdString()));

        const QString target = dir_->filePath(QStringLiteral("optimized.pdf"));
        const OptimizeResult result =
            optimizeDocument(doc.handle(), target.toStdString(), /*signatureCount=*/0);
        QVERIFY2(result.ok(), result.message.c_str());

        const QByteArray optimizedBytes = readAll(target);
        QVERIFY(!optimizedBytes.isEmpty());

        // 用內容而非物件編號判定：整份重寫本來就可能重新編號存活的物件
        // （incremental_saver.h 對 saveAsCopy 的說明：「會重排物件編號」），
        // 所以「孤兒物件的舊編號在輸出裡消失」不能單獨當證據——那個編號
        // 也可能剛好被某個存活物件重新用掉。真正能證明可達性標記發生的，
        // 是這個獨一無二的字典鍵值完全從輸出的位元組裡消失。
        QVERIFY2(!optimizedBytes.contains("AliothTestOrphan"),
                 "孤兒物件的內容在最佳化之後仍然存在於輸出檔裡，PDFium 的整份重寫"
                 "沒有做可達性標記，optimize_saver.h 檔頭「移除未使用物件」的假設不成立");

        const auto qpdf = alioth::test::runQpdfCheck(target);
        if (qpdf.status == alioth::test::QpdfStatus::NotAvailable) {
            QWARN(alioth::test::qpdfSkipReason().constData());
        } else {
            QVERIFY2(qpdf.clean(), alioth::test::describeQpdfFailure(
                                       QStringLiteral("最佳化輸出"), qpdf).constData());
        }
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_APPLESS_MAIN(TestOptimizeSaver)
#include "test_optimize_saver.moc"
