// 掃描頁去斜與增強的端到端測試（WBS 14，PRD-ENH-002）。
//
// 純演算法的部分在 test_image_ops；這裡驗的是「接起來之後仍然是對的」：
// 角度的符號在 PDF 寫入這一段有兩次翻轉的機會（旋轉方向、影像的 Y 軸），
// 兩次都寫反的話單元測試全綠而輸出的頁面歪得更厲害。
// 因此這一條的判準是把輸出重新渲染出來再測一次傾斜。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/enhance/image_codec.h"
#include "engine/enhance/image_ops.h"
#include "engine/enhance/page_rasterizer.h"
#include "engine/enhance/scan_enhancer.h"
#include "engine/objects/incremental_appender.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::enhance;

namespace {

constexpr std::int32_t kEdge = 400;

engine::PixelBuffer syntheticScan(double tiltDegrees) {
    engine::PixelBuffer page = makeBuffer(kEdge, kEdge, 255);
    for (std::int32_t y = 24; y < kEdge - 24; y += 18) {
        for (std::int32_t dy = 0; dy < 6; ++dy) {
            std::uint8_t* row = page.scanline(y + dy);
            for (std::int32_t x = 40; x < kEdge - 40; ++x) {
                row[x * 4 + 0] = 0;
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0;
            }
        }
    }
    return tiltDegrees == 0.0 ? std::move(page) : rotate(page, tiltDegrees, 255);
}

// 單一整頁影像的掃描件——這正是本功能的目標語料。
std::string scannedDocument(const EncodedImage& image) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Contents 4 0 R "
        "/Resources << /XObject << /Im0 5 0 R >> >> >>");

    const std::string content = "q\n400 0 0 400 0 0 cm\n/Im0 Do\nQ\n";
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                      "endstream");
    objects.push_back("<< /Type /XObject /Subtype /Image /Width " + std::to_string(image.width) +
                      " /Height " + std::to_string(image.height) + " /ColorSpace /" +
                      image.colorSpace + " /BitsPerComponent 8 /Filter /" + image.filter +
                      " /Length " + std::to_string(image.data.size()) + " >>\nstream\n" +
                      image.data + "\nendstream");

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

std::string tiltedScanDocument(double tiltDegrees) {
    const EncodedImage encoded = encodeFlate(syntheticScan(tiltDegrees));
    return scannedDocument(encoded);
}

}  // namespace

class TestScanEnhance : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void deskewStraightensTheWrittenPage() {
        const std::string pdf = tiltedScanDocument(3.0);

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        ScanEnhanceSettings settings;
        settings.dpi = 72.0;  // 影像與頁面 1:1，避免重取樣模糊掉判準

        const ScanEnhanceResult result = enhanceScannedPages(appender, {}, settings);
        QVERIFY2(result.ok, qPrintable(QString::fromStdString(result.diagnostic)));
        QCOMPARE(result.pages.size(), std::size_t{1});
        QVERIFY2(result.pages[0].deskewApplied,
                 qPrintable(QString::fromStdString(result.pages[0].deskewNote)));
        QVERIFY(std::abs(result.pages[0].angleDeg - 3.0) < 0.5);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        // 判準：把輸出重新渲染出來，傾斜必須已經消失。
        // 符號寫反的話這裡會量到約 6 度而不是 0 度。
        const RasterizedPage after = renderPage(built.bytes, 0, 72.0);
        QVERIFY(after.ok);
        const domain::enhance::DeskewResult residual = detectSkew(after.pixels);
        QVERIFY2(!residual.detected,
                 qPrintable(QStringLiteral("校正後仍有 %1 度傾斜").arg(residual.angleDeg)));
    }

    void straightPageIsLeftUnrotated() {
        // 正常的掃描件不得被轉歪。旋轉是有損的，誤判無法回復。
        const std::string pdf = tiltedScanDocument(0.0);

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        ScanEnhanceSettings settings;
        settings.dpi = 72.0;
        const ScanEnhanceResult result = enhanceScannedPages(appender, {}, settings);
        QVERIFY(result.ok);
        QVERIFY2(!result.pages[0].deskewApplied,
                 qPrintable(QStringLiteral("水平頁被轉了 %1 度").arg(result.pages[0].angleDeg)));
        QVERIFY(!result.pages[0].deskewNote.empty());
    }

    void binarizationReachesTheWrittenPage() {
        const std::string pdf = tiltedScanDocument(0.0);

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        ScanEnhanceSettings settings;
        settings.dpi = 72.0;
        settings.deskewEnabled = false;
        settings.enhancement.binarize = domain::enhance::BinarizeMode::Otsu;
        settings.compression.codec = domain::enhance::ImageCodec::Flate;  // 無損才驗得到兩值

        QVERIFY(enhanceScannedPages(appender, {}, settings).ok);
        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        const RasterizedPage page = renderPage(built.bytes, 0, 72.0);
        QVERIFY(page.ok);
        int midtones = 0;
        for (std::int32_t y = 0; y < page.pixels.height(); ++y) {
            const std::uint8_t* row =
                page.pixels.data() + page.pixels.stride() * static_cast<std::size_t>(y);
            for (std::int32_t x = 0; x < page.pixels.width(); ++x) {
                const int value = row[x * 4];
                if (value > 24 && value < 231) ++midtones;
            }
        }
        // 渲染時的重取樣會在黑白交界處產生少量中間值，但整體必須壓倒性地是兩值。
        const int total = page.pixels.width() * page.pixels.height();
        QVERIFY2(midtones * 20 < total,
                 qPrintable(QStringLiteral("中間色調 %1 / %2，二值化沒有生效")
                                .arg(midtones).arg(total)));
    }

    void outputPassesQpdfCheck() {
        const std::string pdf = tiltedScanDocument(2.0);
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        ScanEnhanceSettings settings;
        settings.dpi = 72.0;
        settings.enhancement.contrast = 25.0;
        settings.enhancement.brightness = 10.0;
        QVERIFY(enhanceScannedPages(appender, {}, settings).ok);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        const QString path = dir_->filePath(QStringLiteral("enhanced.pdf"));
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

QTEST_MAIN(TestScanEnhance)
#include "test_scan_enhance.moc"
