// 頁面匯出為點陣影像的測試（WP27，PRD-IO-007）。

#include <QtTest>

#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>

#include "engine/iosec/page_image_export.h"
#include "pdf_fixture.h"

using namespace alioth::engine::iosec;

class TestPageImageExport : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        const QByteArray pdf = alioth::test::makeSinglePagePdf();
        pdfBytes_.assign(pdf.constData(), pdf.constData() + pdf.size());
    }

    void cleanup() { dir_.reset(); }

    void exportsPngAtRequestedDpi() {
        const QString path = dir_->filePath(QStringLiteral("page.png"));
        ImageExportOptions options;
        options.dpi = 300.0;
        options.format = ImageExportFormat::Png;

        const ImageExportResult result = exportPageImage(pdfBytes_, 0, path.toStdString(), options);
        QVERIFY2(result.ok(), result.message.c_str());
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(result.bytesWritten > 0);

        // 來源頁是 200×400 點（1 點 = 1/72 吋）。300 dpi 下應為
        // 200/72*300 ≈ 833、400/72*300 ≈ 1667 像素，容許取整誤差。
        QImageReader reader(path);
        const QImage image = reader.read();
        QVERIFY(!image.isNull());
        QVERIFY(qAbs(image.width() - 833) <= 2);
        QVERIFY(qAbs(image.height() - 1667) <= 2);
    }

    void exportsJpegWithQuality() {
        const QString path = dir_->filePath(QStringLiteral("page.jpg"));
        ImageExportOptions options;
        options.dpi = 96.0;
        options.format = ImageExportFormat::Jpeg;
        options.jpegQuality = 80;

        const ImageExportResult result = exportPageImage(pdfBytes_, 0, path.toStdString(), options);
        QVERIFY2(result.ok(), result.message.c_str());
        QVERIFY(QFileInfo::exists(path));
    }

    void exportsTiffWhenPluginAvailable() {
        const QString path = dir_->filePath(QStringLiteral("page.tiff"));
        ImageExportOptions options;
        options.dpi = 150.0;
        options.format = ImageExportFormat::Tiff;

        if (!isFormatSupported(ImageExportFormat::Tiff)) {
            QSKIP("這台機器的 Qt 部署沒有 qtiff 外掛，見 page_image_export.h 的降級說明");
        }
        const ImageExportResult result = exportPageImage(pdfBytes_, 0, path.toStdString(), options);
        QVERIFY2(result.ok(), result.message.c_str());
        QVERIFY(QFileInfo::exists(path));
    }

    void invalidPageIndexIsRejected() {
        const QString path = dir_->filePath(QStringLiteral("page.png"));
        const ImageExportResult result =
            exportPageImage(pdfBytes_, /*pageIndex=*/99, path.toStdString(), {});
        QVERIFY(!result.ok());
        QCOMPARE(result.status, ImageExportStatus::RenderFailed);
        QVERIFY(!QFileInfo::exists(path));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    std::string pdfBytes_;
};

QTEST_APPLESS_MAIN(TestPageImageExport)
#include "test_page_image_export.moc"
