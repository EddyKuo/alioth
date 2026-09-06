// 背景與點陣化的 PDF 寫入測試（WBS 14，PRD-ENH-001 / 003）。
//
// 驗證方式刻意用「渲染取像素」而不是比對內容串流的位元組：背景在上或在下，
// 位元組層的差別只是陣列裡的順序，看起來都合理；而畫面上的差別是
// 「整頁被蓋掉」與「正常」。這一條只有像素答得出來。
//
// 結構正確性另外交給 qpdf——我們自己的檢視器（PDFium）對壞掉的 xref 與
// /Length 容忍度很高，讀得回來完全不能證明檔案是對的。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/enhance/background_writer.h"
#include "engine/enhance/image_codec.h"
#include "engine/enhance/image_ops.h"
#include "engine/enhance/page_rasterizer.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/text/text_extractor.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::enhance;

namespace {

// 200 × 200 的一頁：中央一塊紅色方塊、左上一行文字、外加一個方框註解。
// 三樣東西各自對應一條驗收：紅色方塊驗背景在下、文字驗點陣化之後文字層變空、
// 註解驗點陣化不會把標記一起吃掉。
std::string fixtureDocument() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R] >>");

    const std::string content =
        "1 0 0 rg\n50 50 100 100 re\nf\nBT\n/F1 18 Tf\n15 170 Td\n(Alioth) Tj\nET\n";
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                      "endstream");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Square /Rect [10 10 40 40] /F 4 /C [0 0 1] >>");

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

struct Rgb {
    int r{0};
    int g{0};
    int b{0};
};

Rgb pixelAt(const engine::PixelBuffer& buffer, std::int32_t x, std::int32_t y) {
    const std::uint8_t* row = buffer.data() + buffer.stride() * static_cast<std::size_t>(y);
    return Rgb{row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
}

}  // namespace

class TestEnhancePdf : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        source_ = fixtureDocument();
    }

    void fixtureRendersAsExpected() {
        // 後面每一條都拿這張圖當基準，所以基準本身要先驗過：
        // 基準錯了，之後的比對全都在比較兩個錯誤。
        const RasterizedPage page = renderPage(source_, 0, 72.0);
        QVERIFY2(page.ok, qPrintable(QString::fromStdString(page.diagnostic)));
        QCOMPARE(page.pixels.width(), 200);

        const Rgb centre = pixelAt(page.pixels, 100, 100);
        QVERIFY(centre.r > 200 && centre.g < 60 && centre.b < 60);

        const Rgb corner = pixelAt(page.pixels, 5, 195);
        QVERIFY(corner.r > 240 && corner.g > 240 && corner.b > 240);
    }

    void solidBackgroundGoesUnderExistingContent() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::BackgroundSpec spec;
        spec.source = domain::enhance::BackgroundSource::SolidColor;
        spec.color = domain::ColorRgb{0.0, 0.0, 1.0};
        spec.fit = domain::enhance::BackgroundFit::Stretch;

        const BackgroundResult added = addBackground(appender, spec);
        QVERIFY2(added.ok, qPrintable(QString::fromStdString(added.diagnostic)));
        QCOMPARE(added.pagesChanged.size(), std::size_t{1});

        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, qPrintable(QString::fromStdString(built.diagnostic)));

        const RasterizedPage page = renderPage(built.bytes, 0, 72.0);
        QVERIFY(page.ok);

        // 頁面原本的紅色方塊必須還看得見。看不見就代表背景畫在內容之上，
        // 那是「新增背景」最典型也最難從程式碼看出來的錯誤。
        const Rgb centre = pixelAt(page.pixels, 100, 100);
        QVERIFY2(centre.r > 200 && centre.b < 60,
                 qPrintable(QStringLiteral("中央像素為 (%1,%2,%3)，紅色方塊被背景蓋掉了")
                                .arg(centre.r).arg(centre.g).arg(centre.b)));

        // 原本空白的角落則變成背景色。
        const Rgb corner = pixelAt(page.pixels, 5, 195);
        QVERIFY2(corner.b > 200 && corner.r < 60,
                 qPrintable(QStringLiteral("角落像素為 (%1,%2,%3)，背景沒有生效")
                                .arg(corner.r).arg(corner.g).arg(corner.b)));
    }

    void backgroundContentStreamComesFirst() {
        // 像素驗的是結果，這一條驗的是機制：新的內容串流必須排在 /Contents
        // 陣列的第一項。兩條一起才能區分「順序對」與「剛好看起來對」。
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::BackgroundSpec spec;
        spec.color = domain::ColorRgb{0.9, 0.9, 0.2};
        QVERIFY(addBackground(appender, spec).ok);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        QCOMPARE(output.pages().size(), std::size_t{1});

        const engine::objects::PdfObject page = output.object(output.pages()[0].number);
        const engine::objects::PdfDictionary* dict = page.asDictionary();
        QVERIFY(dict != nullptr);
        const engine::objects::PdfObject* contents = dict->find("Contents");
        QVERIFY(contents != nullptr);
        const engine::objects::PdfArray* array = contents->asArray();
        QVERIFY2(array != nullptr, "背景寫入後 /Contents 應為陣列");
        QCOMPARE(array->size(), std::size_t{2});
        // 原本的內容串流是 4 0 R，背景是後配的號碼，因此背景一定不是 4。
        QVERIFY((*array)[0].asRef().number != 4);
        QCOMPARE((*array)[1].asRef().number, 4);
    }

    void imageBackgroundIsAlsoDrawnUnderContent() {
        // 影像背景與純色背景走不同的分支（多了 XObject 資源與繪製矩陣），
        // 順序寫對一種不代表另一種也對。
        engine::PixelBuffer blue = makeBuffer(64, 64, 0);
        for (std::int32_t y = 0; y < 64; ++y) {
            std::uint8_t* row = blue.scanline(y);
            for (std::int32_t x = 0; x < 64; ++x) {
                row[x * 4 + 0] = 255;  // B
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0;
                row[x * 4 + 3] = 255;
            }
        }
        const EncodedImage jpeg = encodeJpeg(blue, 90);
        QVERIFY(jpeg.ok);

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::BackgroundSpec spec;
        spec.source = domain::enhance::BackgroundSource::Image;
        spec.fit = domain::enhance::BackgroundFit::Stretch;
        spec.imageBytes.assign(jpeg.data.begin(), jpeg.data.end());

        const BackgroundResult added = addBackground(appender, spec);
        QVERIFY2(added.ok, qPrintable(QString::fromStdString(added.diagnostic)));
        QVERIFY(added.imageObject > 0);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        const RasterizedPage page = renderPage(built.bytes, 0, 72.0);
        QVERIFY(page.ok);
        const Rgb centre = pixelAt(page.pixels, 100, 100);
        QVERIFY2(centre.r > 180 && centre.b < 80, "紅色方塊被影像背景蓋掉了");
        const Rgb corner = pixelAt(page.pixels, 5, 195);
        QVERIFY2(corner.b > 180 && corner.r < 80, "影像背景沒有生效");

        checkWithQpdf(built.bytes, QStringLiteral("image_background.pdf"));
    }

    void backgroundOutputPassesQpdfCheck() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::BackgroundSpec spec;
        spec.color = domain::ColorRgb{0.2, 0.4, 0.9};
        spec.opacity = 0.35;  // 走 /ExtGState 的路徑，資源字典的寫法一起驗
        QVERIFY(addBackground(appender, spec).ok);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);
        checkWithQpdf(built.bytes, QStringLiteral("background.pdf"));
    }

    void rasterizeEmptiesTextLayerButKeepsAnnotations() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::RasterizeSettings settings;
        settings.dpi = 96.0;
        const RasterizeResult result = rasterizePages(appender, {}, settings);
        QVERIFY2(result.ok, qPrintable(QString::fromStdString(result.diagnostic)));
        QCOMPARE(result.pagesChanged.size(), std::size_t{1});

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        // 註解：點陣化的是內容不是標記，/Annots 必須原封不動。
        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        const engine::objects::PdfObject page = output.object(output.pages()[0].number);
        const engine::objects::PdfObject* annots = page.asDictionary()->find("Annots");
        QVERIFY2(annots != nullptr, "點陣化之後 /Annots 不見了");
        const engine::objects::PdfObject resolved = output.resolve(*annots);
        QVERIFY(resolved.asArray() != nullptr);
        QCOMPARE(resolved.asArray()->size(), std::size_t{1});

        // 文字層：內容已經變成一張圖，可選取的文字必須歸零。
        const QString path = dir_->filePath(QStringLiteral("rasterized.pdf"));
        writeFile(path, built.bytes);

        QCOMPARE(textCharacterCount(path), 0);
        QVERIFY2(textCharacterCount(sourcePath()) > 0,
                 "原始語料本來就沒有文字，這條測試不成立");
    }

    void rasterizedPageStillLooksTheSame() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::RasterizeSettings settings;
        settings.dpi = 150.0;
        settings.compression.codec = domain::enhance::ImageCodec::Flate;
        QVERIFY(rasterizePages(appender, {0}, settings).ok);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        const RasterizedPage page = renderPage(built.bytes, 0, 72.0);
        QVERIFY(page.ok);
        const Rgb centre = pixelAt(page.pixels, 100, 100);
        QVERIFY2(centre.r > 190 && centre.b < 70,
                 qPrintable(QStringLiteral("點陣化後中央像素為 (%1,%2,%3)")
                                .arg(centre.r).arg(centre.g).arg(centre.b)));
        const Rgb corner = pixelAt(page.pixels, 5, 195);
        QVERIFY(corner.r > 230 && corner.g > 230 && corner.b > 230);
    }

    void rasterizeOutputPassesQpdfCheck() {
        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(source_), engine::objects::SourceStatus::Ok);

        domain::enhance::RasterizeSettings settings;
        settings.dpi = 96.0;
        QVERIFY(rasterizePages(appender, {}, settings).ok);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);
        checkWithQpdf(built.bytes, QStringLiteral("rasterize_check.pdf"));
    }

private:
    void writeFile(const QString& path, const std::string& bytes) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes.data(), static_cast<qint64>(bytes.size()));
        file.close();
    }

    QString sourcePath() {
        const QString path = dir_->filePath(QStringLiteral("source.pdf"));
        if (!QFile::exists(path)) writeFile(path, source_);
        return path;
    }

    // PDFium 的文字擷取跑在自己的執行緒上，這裡同步等它做完。
    std::int32_t textCharacterCount(const QString& path) {
        engine::text::TextExtractor extractor;
        std::int32_t count = -1;
        extractor.open(path.toStdString(), {}, [](domain::DocumentError) {});
        extractor.waitForIdle();
        extractor.withTextPage(0, [&count](const engine::text::TextPage* page) {
            count = page == nullptr ? -1 : engine::text::charCount(*page);
        });
        extractor.waitForIdle();
        return count;
    }

    void checkWithQpdf(const std::string& bytes, const QString& name) {
        const QString path = dir_->filePath(name);
        writeFile(path, bytes);
        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

    std::unique_ptr<QTemporaryDir> dir_;
    std::string source_;
};

QTEST_MAIN(TestEnhancePdf)
#include "test_enhance_pdf.moc"
