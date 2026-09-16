// 影像重壓縮（WBS 14，PRD-ENH-004）。
//
// 這支測試的重點不是「壓得多小」，而是**什麼時候不該換**。重壓後變大是常態，
// 而一個把檔案愈壓愈大的功能比沒有這個功能糟糕。因此兩個方向都要驗：
// 該換的有換、不該換的原圖一個位元組都沒動。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/enhance/image_codec.h"
#include "engine/enhance/image_ops.h"
#include "engine/enhance/image_recompressor.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::enhance;
using domain::enhance::RecompressDecision;
using domain::enhance::RecompressSettings;

namespace {

// 連續色調的圖：JPEG 壓得下去，無損壓不太動。用來造「該換」的情境。
engine::PixelBuffer gradientImage(std::int32_t size) {
    engine::PixelBuffer image = makeBuffer(size, size, 255);
    for (std::int32_t y = 0; y < size; ++y) {
        std::uint8_t* row = image.scanline(y);
        for (std::int32_t x = 0; x < size; ++x) {
            row[x * 4 + 0] = static_cast<std::uint8_t>(x * 255 / size);
            row[x * 4 + 1] = static_cast<std::uint8_t>(y * 255 / size);
            row[x * 4 + 2] = static_cast<std::uint8_t>((x + y) * 255 / (2 * size));
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

// 高熵雜訊：無損編碼一定比 JPEG 大。用來造「不該換」的情境。
engine::PixelBuffer noiseImage(std::int32_t size) {
    engine::PixelBuffer image = makeBuffer(size, size, 0);
    std::uint32_t state = 987654321u;
    for (std::int32_t y = 0; y < size; ++y) {
        std::uint8_t* row = image.scanline(y);
        for (std::int32_t x = 0; x < size; ++x) {
            state = state * 1664525u + 1013904223u;
            row[x * 4 + 0] = static_cast<std::uint8_t>(state >> 8);
            row[x * 4 + 1] = static_cast<std::uint8_t>(state >> 16);
            row[x * 4 + 2] = static_cast<std::uint8_t>(state >> 24);
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

struct ImageEntry {
    std::string name;
    EncodedImage encoded;
    bool withSoftMask{false};
};

// 一頁、若干張影像的最小文件。影像以間接參照掛在 /Resources /XObject 下，
// 與真實掃描件的結構一致。
std::string documentWithImages(const std::vector<ImageEntry>& entries) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("");  // 第 3 號是頁面，資源名單湊齊後再回填
    objects.push_back("");  // 第 4 號是內容串流

    std::string resources;
    std::string content;
    int nextNumber = 5;
    for (const ImageEntry& entry : entries) {
        int maskNumber = 0;
        if (entry.withSoftMask && !entry.encoded.softMaskData.empty()) {
            maskNumber = nextNumber++;
            objects.push_back("<< /Type /XObject /Subtype /Image /Width " +
                              std::to_string(entry.encoded.width) + " /Height " +
                              std::to_string(entry.encoded.height) +
                              " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode "
                              "/Length " +
                              std::to_string(entry.encoded.softMaskData.size()) + " >>\nstream\n" +
                              entry.encoded.softMaskData + "\nendstream");
        }

        const int imageNumber = nextNumber++;
        std::string dict = "<< /Type /XObject /Subtype /Image /Width " +
                           std::to_string(entry.encoded.width) + " /Height " +
                           std::to_string(entry.encoded.height) + " /ColorSpace /" +
                           entry.encoded.colorSpace + " /BitsPerComponent 8";
        if (!entry.encoded.filter.empty()) dict += " /Filter /" + entry.encoded.filter;
        if (maskNumber > 0) dict += " /SMask " + std::to_string(maskNumber) + " 0 R";
        dict += " /Length " + std::to_string(entry.encoded.data.size()) + " >>\nstream\n" +
                entry.encoded.data + "\nendstream";
        objects.push_back(dict);

        resources += "/" + entry.name + " " + std::to_string(imageNumber) + " 0 R ";
        content += "q 100 0 0 100 10 10 cm /" + entry.name + " Do Q\n";
    }

    objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
                 "/Resources << /XObject << " + resources + ">> >> >>";
    objects[3] = "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                 "endstream";

    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

std::string streamDataOf(const std::string& pdf, int objectNumber) {
    engine::objects::PdfSourceDocument document;
    if (document.open(pdf) != engine::objects::SourceStatus::Ok) return {};
    const engine::objects::PdfObject object = document.object(objectNumber);
    const engine::objects::PdfStream* stream = object.asStream();
    return stream == nullptr ? std::string{} : stream->data;
}

}  // namespace

class TestRecompress : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void replacesImageWhenResultIsSmaller() {
        const EncodedImage lossless = encodeFlate(gradientImage(160));
        QVERIFY(lossless.ok);
        const std::string pdf = documentWithImages({{"Im0", lossless, false}});

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        RecompressSettings settings;
        settings.compression.codec = domain::enhance::ImageCodec::Jpeg;
        settings.compression.jpegQuality = 40;

        const RecompressResult result = recompressImages(appender, {}, settings);
        QVERIFY2(result.ok, qPrintable(QString::fromStdString(result.diagnostic)));
        QCOMPARE(result.images.size(), std::size_t{1});
        QCOMPARE(result.images[0].decision, RecompressDecision::Replaced);
        QVERIFY(result.images[0].newBytes < result.images[0].originalBytes);
        QVERIFY(result.savedBytes() > 0);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        const std::string before = streamDataOf(pdf, result.images[0].objectNumber);
        const std::string after = streamDataOf(built.bytes, result.images[0].objectNumber);
        QVERIFY(!after.empty());
        QVERIFY2(after.size() < before.size(), "替換後的串流沒有比較小");
    }

    void keepsOriginalWhenRecompressionWouldGrow() {
        // 雜訊照片 + 無損編碼：這是重壓縮最常見的失敗情境，
        // 而失敗的正確表現是「什麼都不做並說明原因」。
        const EncodedImage jpeg = encodeJpeg(noiseImage(128), 30);
        QVERIFY(jpeg.ok);
        const std::string pdf = documentWithImages({{"Im0", jpeg, false}});

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        RecompressSettings settings;
        settings.compression.codec = domain::enhance::ImageCodec::Flate;

        const RecompressResult result = recompressImages(appender, {}, settings);
        QVERIFY(result.ok);
        QCOMPARE(result.images.size(), std::size_t{1});
        QCOMPARE(result.images[0].decision, RecompressDecision::KeptLarger);
        QCOMPARE(result.images[0].savedBytes(), std::int64_t{0});
        // 報告仍然要帶出重壓後的大小，使用者才知道為什麼沒省到。
        QVERIFY(result.images[0].newBytes > result.images[0].originalBytes);

        // 沒有任何物件被改寫，附加段因此是空的：原圖必須一個位元組都沒動。
        QVERIFY2(!appender.hasPendingObjects(), "不該替換卻寫出了新物件");
    }

    void reportsSavingBelowThresholdSeparately() {
        // 「有變小但不值得」與「變大」是兩件事，使用者的下一步也不同
        // （前者可以調低品質再試），因此不能混成同一個結果。
        const EncodedImage lossless = encodeFlate(gradientImage(160));
        QVERIFY(lossless.ok);
        const std::string pdf = documentWithImages({{"Im0", lossless, false}});

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        RecompressSettings settings;
        settings.compression.codec = domain::enhance::ImageCodec::Jpeg;
        settings.compression.jpegQuality = 40;
        settings.minSavingRatio = 0.999;  // 幾乎不可能達成

        const RecompressResult result = recompressImages(appender, {}, settings);
        QVERIFY(result.ok);
        QCOMPARE(result.images[0].decision, RecompressDecision::KeptBelowSaving);
        QVERIFY(!appender.hasPendingObjects());
    }

    void softMaskIsRecompressedTogetherWithTheImage() {
        // 只換彩色資料而留下舊的 /SMask，尺寸一旦不同透明度就整片錯位。
        engine::PixelBuffer translucent = gradientImage(160);
        for (std::int32_t y = 0; y < translucent.height(); ++y) {
            std::uint8_t* row = translucent.scanline(y);
            for (std::int32_t x = 0; x < translucent.width(); ++x) {
                row[x * 4 + 3] = static_cast<std::uint8_t>(x < 80 ? 0 : 255);
            }
        }
        const EncodedImage lossless = encodeFlate(translucent);
        QVERIFY(lossless.ok);
        QVERIFY(!lossless.softMaskData.empty());

        const std::string pdf = documentWithImages({{"Im0", lossless, true}});
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        RecompressSettings settings;
        settings.compression.codec = domain::enhance::ImageCodec::Jpeg;
        settings.compression.jpegQuality = 40;

        const RecompressResult result = recompressImages(appender, {}, settings);
        QVERIFY(result.ok);
        QCOMPARE(result.images.size(), std::size_t{1});
        QCOMPARE(result.images[0].decision, RecompressDecision::Replaced);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        const engine::objects::PdfObject image = output.object(result.images[0].objectNumber);
        const engine::objects::PdfDictionary* dict = image.asDictionary();
        QVERIFY(dict != nullptr);
        const engine::objects::PdfObject* smask = dict->find("SMask");
        QVERIFY2(smask != nullptr, "替換後 /SMask 不見了，透明度會整片消失");

        const engine::objects::PdfObject mask = output.resolve(*smask);
        const engine::objects::PdfDictionary* maskDict = mask.asDictionary();
        QVERIFY(maskDict != nullptr);
        QCOMPARE(output.resolve(*maskDict->find("Width")).asInteger(),
                 output.resolve(*dict->find("Width")).asInteger());
        QCOMPARE(output.resolve(*maskDict->find("Height")).asInteger(),
                 output.resolve(*dict->find("Height")).asInteger());
    }

    void preservesMaskBytes_data() {
        QTest::addColumn<bool>("damagedMask");
        QTest::newRow("intact-mask") << false;
        // 解不開的遮罩最危險：一旦被當成「沒有遮罩」而略過，替換後的影像
        // 就整片不透明，而那不會有任何錯誤訊息。
        QTest::newRow("undecodable-mask") << true;
    }

    void preservesMaskBytes() {
        QFETCH(bool, damagedMask);
        auto pixels = gradientImage(160);
        for (int y = 0; y < 160; ++y) {
            for (int x = 0; x < 160; ++x) pixels.scanline(y)[x * 4 + 3] = 100;
        }
        EncodedImage image = encodeFlate(pixels);
        QVERIFY(image.ok);
        QVERIFY(!image.softMaskData.empty());
        if (damagedMask) image.softMaskData = "not a valid Flate stream";
        const std::string pdf = documentWithImages({{"Im0", image, true}});
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);
        RecompressSettings settings;
        settings.compression.codec = domain::enhance::ImageCodec::Jpeg;
        settings.compression.jpegQuality = 40;
        const auto result = recompressImages(appender, {}, settings);
        QVERIFY(result.ok);
        QCOMPARE(result.replacedCount(), std::size_t{1});
        const auto built = appender.build();
        QVERIFY(built.ok);
        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        const auto replacement = output.object(result.images.front().objectNumber);
        const auto* mask = replacement.asStream()->dict.find("SMask");
        QVERIFY(mask != nullptr);
        QCOMPARE(mask->asRef().number, 5);
        QCOMPARE(streamDataOf(built.bytes, 5), image.softMaskData);
        QCOMPARE(result.images.front().originalBytes,
                 static_cast<std::int64_t>(image.data.size()));
    }

    void tinyImagesAreSkipped() {
        const EncodedImage small = encodeFlate(gradientImage(16));
        QVERIFY(small.ok);
        const std::string pdf = documentWithImages({{"Im0", small, false}});

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        const RecompressResult result = recompressImages(appender, {}, {});
        QVERIFY(result.ok);
        QCOMPARE(result.images.size(), std::size_t{1});
        QCOMPARE(result.images[0].decision, RecompressDecision::SkippedTooSmall);
    }

    void decisionBoundariesAreExact() {
        RecompressSettings settings;
        settings.minSavingRatio = 0.05;
        QCOMPARE(domain::enhance::decideReplacement(1000, 1000, settings),
                 RecompressDecision::KeptLarger);
        QCOMPARE(domain::enhance::decideReplacement(1000, 1001, settings),
                 RecompressDecision::KeptLarger);
        QCOMPARE(domain::enhance::decideReplacement(1000, 950, settings),
                 RecompressDecision::Replaced);
        QCOMPARE(domain::enhance::decideReplacement(1000, 951, settings),
                 RecompressDecision::KeptBelowSaving);
        QCOMPARE(domain::enhance::decideReplacement(0, 100, settings),
                 RecompressDecision::Failed);
    }

    void outputPassesQpdfCheck() {
        const EncodedImage lossless = encodeFlate(gradientImage(160));
        const std::string pdf = documentWithImages({{"Im0", lossless, false}});

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        RecompressSettings settings;
        settings.compression.codec = domain::enhance::ImageCodec::Jpeg;
        settings.compression.jpegQuality = 40;
        QVERIFY(recompressImages(appender, {}, settings).ok);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        const QString path = dir_->filePath(QStringLiteral("recompressed.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(built.bytes.data(), static_cast<qint64>(built.bytes.size()));
        file.close();

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestRecompress)
#include "test_recompress.moc"
