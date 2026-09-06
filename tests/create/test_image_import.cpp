// 從影像建立 PDF 的測試（PRD-IO-009，WBS 15）。
//
// 語料一律自產：JPEG 用 Qt 現場編碼，其餘用手填的像素緩衝區。不放固定檔案
// 的理由與 SDD §8 一致——測試要能在乾淨的機器上跑，而且「來源位元組是什麼」
// 必須在測試裡看得見，否則「未被重新編碼」這條斷言就無從解讀。

#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include <string>
#include <vector>

#include "create_test_support.h"
#include "domain/document_source.h"
#include "engine/create/image_to_pdf.h"

using namespace alioth::domain::create;
using namespace alioth::engine::create;

namespace {

SourceImage makeRgb(int width, int height, double dpi = 96.0) {
    SourceImage image;
    image.width = width;
    image.height = height;
    image.format = ImagePixelFormat::Rgb8;
    image.dpiX = dpi;
    image.dpiY = dpi;
    image.bytes.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0; i < image.bytes.size(); i += 3) {
        image.bytes[i] = static_cast<std::uint8_t>(i % 251);
        image.bytes[i + 1] = 0x40;
        image.bytes[i + 2] = 0x90;
    }
    return image;
}

SourceImage makeRgba(int width, int height) {
    SourceImage image;
    image.width = width;
    image.height = height;
    image.format = ImagePixelFormat::Rgba8;
    image.bytes.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < image.bytes.size(); i += 4) {
        image.bytes[i] = 0xFF;
        image.bytes[i + 1] = 0x00;
        image.bytes[i + 2] = 0x00;
        // 左半透明、右半不透明，讓遮罩不是常數——常數遮罩會被壓縮成幾個
        // 位元組，看不出資料有沒有真的寫進去。
        image.bytes[i + 3] = ((i / 4) % static_cast<std::size_t>(width)) <
                                     static_cast<std::size_t>(width) / 2
                                 ? 0x00
                                 : 0xFF;
    }
    return image;
}

// 現場編碼一張 JPEG，並把同一份位元組同時當成「來源」與「預期值」。
std::vector<std::uint8_t> encodeJpeg(int width, int height) {
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, qRgb((x * 7) % 256, (y * 11) % 256, 128));
        }
    }
    QByteArray buffer;
    QBuffer device(&buffer);
    device.open(QIODevice::WriteOnly);
    image.save(&device, "JPEG", 85);
    device.close();
    return std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(buffer.constData()),
        reinterpret_cast<const std::uint8_t*>(buffer.constData()) + buffer.size());
}

bool contains(const std::string& haystack, const std::vector<std::uint8_t>& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    const std::string pattern(reinterpret_cast<const char*>(needle.data()), needle.size());
    return haystack.find(pattern) != std::string::npos;
}

}  // namespace

class TestImageImport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    // 一張影像一頁，不合併也不分割：PRD-IO-009 的語意就是這樣。
    void threeImagesBecomeThreePages() {
        const std::vector<SourceImage> images{makeRgb(40, 30), makeRgb(20, 60), makeRgb(10, 10)};
        const ImageImportResult result = createPdfFromImages(images);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.pageCount, std::size_t{3});

        const QString path = alioth::test::create::writeBytes(dir_->path(),
                                                              QStringLiteral("three.pdf"),
                                                              result.bytes);
        QVERIFY(!path.isEmpty());
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 3);
    }

    // 300 dpi 的 600×300 影像等於 2×1 吋，也就是 144×72 點。
    void pageSizeFollowsImageDpi() {
        ImageImportOptions options;
        options.sizing = ImagePageSizing::FromImageDpi;

        const ImageImportResult result = createPdfFromImages({makeRgb(600, 300, 300.0)}, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.placements.size(), std::size_t{1});
        QVERIFY(qFuzzyCompare(result.placements[0].pageWidthPt, 144.0));
        QVERIFY(qFuzzyCompare(result.placements[0].pageHeightPt, 72.0));

        const QString path = alioth::test::create::writeBytes(dir_->path(),
                                                              QStringLiteral("dpi.pdf"),
                                                              result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        // PDFium 讀回來的 MediaBox 必須與我們算的一致，否則換算只是自說自話。
        QVERIFY(std::abs(opened.pageSizes[0].width - 144.0) < 0.01);
        QVERIFY(std::abs(opened.pageSizes[0].height - 72.0) < 0.01);
    }

    // 固定紙張時影像等比縮放並置中，且預設不放大。
    void fixedPaperCentersWithoutUpscaling() {
        ImageImportOptions options;
        options.sizing = ImagePageSizing::FixedPaper;
        options.paper = kPaperLetter;
        options.marginPt = 36.0;

        const ImageImportResult result = createPdfFromImages({makeRgb(72, 72, 72.0)}, options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        const ImagePlacement& placement = result.placements[0];
        QVERIFY(qFuzzyCompare(placement.pageWidthPt, 612.0));
        QVERIFY(qFuzzyCompare(placement.pageHeightPt, 792.0));
        QVERIFY(std::abs(placement.rect.width() - 72.0) < 0.01);
        QVERIFY(std::abs(placement.rect.left - (612.0 - 72.0) / 2.0) < 0.01);
        QVERIFY(std::abs(placement.rect.bottom - (792.0 - 72.0) / 2.0) < 0.01);
    }

    // 本工作包最實質的一條：JPEG 進來什麼樣，PDF 裡就是什麼樣。
    void jpegIsEmbeddedWithoutRecoding() {
        const std::vector<std::uint8_t> jpeg = encodeJpeg(64, 48);
        QVERIFY(jpeg.size() > 128);
        const JpegProbe probe = probeJpeg(jpeg);
        QVERIFY(probe.ok);
        QCOMPARE(probe.width, 64);
        QCOMPARE(probe.height, 48);

        SourceImage image;
        image.width = 64;
        image.height = 48;
        image.format = ImagePixelFormat::JpegEncoded;
        image.bytes = jpeg;

        const ImageImportResult result = createPdfFromImages({image});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY2(result.bytes.find("/DCTDecode") != std::string::npos,
                 "JPEG 應以 DCTDecode 直接嵌入");
        QVERIFY2(contains(result.bytes, jpeg),
                 "PDF 裡找不到與來源逐位元組相同的 JPEG，代表資料被重新編碼過");

        const QString path = alioth::test::create::writeBytes(dir_->path(),
                                                              QStringLiteral("jpeg.pdf"),
                                                              result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("JPEG 匯入"));
    }

    // 宣告的尺寸與 JPEG 標頭不符時必須失敗。靜默採信其中一邊的結果是
    // 檢視器上的斜條紋，那種畫面沒有人猜得到原因。
    void jpegDimensionMismatchIsRejected() {
        SourceImage image;
        image.width = 100;  // 實際是 64
        image.height = 48;
        image.format = ImagePixelFormat::JpegEncoded;
        image.bytes = encodeJpeg(64, 48);

        const ImageImportResult result = createPdfFromImages({image});
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("不符") != std::string::npos);
    }

    // 含 alpha 必須產生 /SMask，否則透明處在檢視器上會變成黑色。
    void alphaProducesSoftMask() {
        const ImageImportResult result = createPdfFromImages({makeRgba(32, 16)});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.smaskCount, std::size_t{1});
        QVERIFY2(result.bytes.find("/SMask") != std::string::npos, "缺少柔性遮罩");
        QVERIFY2(result.bytes.find("/DeviceGray") != std::string::npos,
                 "遮罩的色彩空間必須是 DeviceGray");

        const QString path = alioth::test::create::writeBytes(dir_->path(),
                                                              QStringLiteral("alpha.pdf"),
                                                              result.bytes);
        const auto opened = alioth::test::create::openWithPdfium(path);
        QVERIFY2(opened.ok, opened.detail.toUtf8().constData());
        QCOMPARE(opened.pageCount, 1);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("含 alpha 的影像匯入"));
    }

    // 沒有 alpha 就不該冒出遮罩：多寫一個 /SMask 會讓檔案變大又多一份
    // 沒人維護的資料。
    void opaqueImageHasNoSoftMask() {
        const ImageImportResult result = createPdfFromImages({makeRgb(16, 16)});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.smaskCount, std::size_t{0});
        QVERIFY(result.bytes.find("/SMask") == std::string::npos);
    }

    void greyscaleUsesDeviceGray() {
        SourceImage image;
        image.width = 8;
        image.height = 8;
        image.format = ImagePixelFormat::Gray8;
        image.bytes.assign(64, 0x77);

        const ImageImportResult result = createPdfFromImages({image});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.bytes.find("/DeviceGray") != std::string::npos);
    }

    // 一張壞掉就整批失敗。少一頁而沒有提示是最難察覺的資料遺失。
    void malformedImageFailsWholeBatch() {
        SourceImage broken;
        broken.width = 10;
        broken.height = 10;
        broken.format = ImagePixelFormat::Rgb8;
        broken.bytes.assign(10, 0);  // 應該要有 300 個位元組

        const ImageImportResult result = createPdfFromImages({makeRgb(8, 8), broken});
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("第 2 張") != std::string::npos);
    }

    void emptyInputIsRejected() {
        const ImageImportResult result = createPdfFromImages({});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void multiPageOutputPassesQpdf() {
        const std::vector<SourceImage> images{makeRgb(40, 30, 150.0), makeRgba(24, 24),
                                              makeRgb(60, 20)};
        const ImageImportResult result = createPdfFromImages(images);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        const QString path = alioth::test::create::writeBytes(dir_->path(),
                                                              QStringLiteral("mixed.pdf"),
                                                              result.bytes);
        ALIOTH_REQUIRE_QPDF_CLEAN(path, QStringLiteral("混合來源的影像匯入"));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestImageImport)
#include "test_image_import.moc"
