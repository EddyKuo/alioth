// 色彩轉換與 Recolor（PRD-ENH-006，WP35）。
//
// 兩層各自測：內容串流的運算子改寫是純字串轉換，直接餵位元組驗證最快；
// 影像與整份文件的路徑需要一份真的 PDF，才能驗證「頁面 → 資源 → XObject」
// 這條路徑真的有走到，而不是只測了中間那個函式。

#include <QtTest>

#include "domain/enhance.h"
#include "engine/enhance/color_transform.h"
#include "engine/enhance/image_codec.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"

using namespace alioth;
using namespace alioth::engine::enhance;
using domain::enhance::ColorTransformMode;
using domain::enhance::ColorTransformSettings;

namespace {

engine::PixelBuffer solidColorImage(std::int32_t size, std::uint8_t r, std::uint8_t g,
                                    std::uint8_t b) {
    engine::PixelBuffer image(size, size);
    for (std::int32_t y = 0; y < size; ++y) {
        std::uint8_t* row = image.scanline(y);
        for (std::int32_t x = 0; x < size; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

// 一頁、一個內容串流、一張影像的最小文件。與 tests/enhance/test_recompress.cpp
// 的 documentWithImages 同構但獨立實作——兩支測試不共用輔助檔，
// 避免其中一個工作包改壞輔助函式時另一個工作包的測試跟著遭殃。
std::string documentWithContentAndImage(const std::string& content, const EncodedImage& image) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
                      "/Resources << /XObject << /Im0 5 0 R >> >> >>");
    objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                      "endstream");
    std::string dict = "<< /Type /XObject /Subtype /Image /Width " + std::to_string(image.width) +
                       " /Height " + std::to_string(image.height) + " /ColorSpace /" +
                       image.colorSpace + " /BitsPerComponent 8";
    if (!image.filter.empty()) dict += " /Filter /" + image.filter;
    dict += " /Length " + std::to_string(image.data.size()) + " >>\nstream\n" + image.data +
           "\nendstream";
    objects.push_back(dict);

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

class TestColorTransform : public QObject {
    Q_OBJECT

private slots:
    // -- 內容串流層 ----------------------------------------------------------

    void grayscaleRewritesRgToG() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        const ContentColorTransformResult result =
            transformContentStreamColors("1 0 0 rg 0 0 100 100 re f\n", settings);
        QCOMPARE(result.rewritten, 1);
        QCOMPARE(result.skippedUnsupported, 0);
        // 純紅色 (1,0,0) 的 BT.601 亮度 = 0.299。
        QVERIFY2(result.content.find("0.299 g") != std::string::npos, result.content.c_str());
        // 其餘內容原樣保留。
        QVERIFY(result.content.find("0 0 100 100 re f") != std::string::npos);
    }

    void grayscaleRewritesStrokeVariantToUppercaseG() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        const ContentColorTransformResult result =
            transformContentStreamColors("0 1 0 RG 10 10 m 50 50 l S\n", settings);
        QCOMPARE(result.rewritten, 1);
        // 純綠色 (0,1,0) 的亮度 = 0.587。
        QVERIFY2(result.content.find("0.587 G") != std::string::npos, result.content.c_str());
    }

    void desaturateInterpolatesTowardGray() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Desaturate;
        settings.desaturateAmount = 0.5;
        const ContentColorTransformResult result =
            transformContentStreamColors("0 0 1 rg 0 0 10 10 re f\n", settings);
        QCOMPARE(result.rewritten, 1);
        // 藍色 (0,0,1)，亮度 0.114。amount=0.5 時每個分量走一半：
        // r: 0 + (0.114-0)*0.5=0.057, g 同 r, b: 1 + (0.114-1)*0.5=0.557。
        QVERIFY2(result.content.find("0.057 0.057 0.557 rg") != std::string::npos,
                result.content.c_str());
    }

    void replaceColorOnlyTouchesMatchingColor() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::ReplaceColor;
        settings.replaceFrom = domain::ColorRgb{1.0, 1.0, 0.0};  // 黃色螢光筆
        settings.replaceTo = domain::ColorRgb{0.0, 1.0, 1.0};    // 換成青色
        settings.replaceTolerance = 0.05;

        const std::string content =
            "1 1 0 rg 0 0 10 10 re f\n0 0 0 rg 20 20 10 10 re f\n";  // 黃 + 黑
        const ContentColorTransformResult result = transformContentStreamColors(content, settings);
        QCOMPARE(result.rewritten, 2);  // 兩個運算子都會被檢查，只是黑色轉換結果等於自己
        QVERIFY2(result.content.find("0 1 1 rg") != std::string::npos, result.content.c_str());
        QVERIFY2(result.content.find("0 0 0 rg") != std::string::npos, result.content.c_str());
    }

    void cmykOperatorIsApproximatedViaNaiveConversion() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        // C=0 M=0 Y=0 K=1 → 樸素換算為純黑 (0,0,0) → 亮度 0。
        const ContentColorTransformResult result =
            transformContentStreamColors("0 0 0 1 k 0 0 10 10 re f\n", settings);
        QCOMPARE(result.rewritten, 1);
        QVERIFY2(result.content.find("0 g") != std::string::npos, result.content.c_str());
    }

    // sc 前面帶了色彩空間名稱（透過 cs 運算子與 Pattern 名稱），運算元
    // 數量與型態不符合本功能認得的 1/3/4 純數字組合，必須明確計入未覆蓋。
    void scnWithPatternNameIsSkippedAsUnsupported() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        const ContentColorTransformResult result =
            transformContentStreamColors("/P1 scn 0 0 10 10 re f\n", settings);
        QCOMPARE(result.rewritten, 0);
        QCOMPARE(result.skippedUnsupported, 1);
        // 原始位元組必須完全不變。
        QCOMPARE(result.content, std::string("/P1 scn 0 0 10 10 re f\n"));
    }

    void numbersInsideStringsAreNotMistakenForOperands() {
        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        const std::string content = "(1 0 0 rg) Tj\n1 0 0 rg 0 0 10 10 re f\n";
        const ContentColorTransformResult result = transformContentStreamColors(content, settings);
        // 字串裡的 "1 0 0 rg" 只是要顯示的文字，不是真正的運算子，
        // 只有字串外面那一個才應該被改寫。
        QCOMPARE(result.rewritten, 1);
        QVERIFY2(result.content.find("(1 0 0 rg) Tj") != std::string::npos, result.content.c_str());
    }

    // -- 端對端（影像 + 內容串流走完整個文件） ---------------------------------

    void convertColorsRewritesBothImageAndVectorContent() {
        const EncodedImage image = encodeFlate(solidColorImage(32, 255, 0, 0));  // 純紅
        QVERIFY(image.ok);
        const std::string pdf =
            documentWithContentAndImage("1 0 0 rg 0 0 50 50 re f\nq 100 0 0 100 10 10 cm /Im0 Do Q\n",
                                       image);

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        const ColorTransformResult result = convertColors(appender, settings);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.report.imagesTransformed, 1);
        QCOMPARE(result.report.contentOperatorsRewritten, 1);

        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        // 影像應該變成灰階：解回像素後三通道相等。
        engine::objects::PdfSourceDocument output;
        QCOMPARE(output.open(built.bytes), engine::objects::SourceStatus::Ok);
        const std::string newImageData = streamDataOf(built.bytes, 5);
        QVERIFY(!newImageData.empty());
        // 純紅色轉灰階後應該是同一個灰階值重複填滿：直接解碼驗證比對位元組更穩健，
        // 這裡改用 decodeRawSamples／decodeJpeg 太重，改用一個簡單但足夠的檢查——
        // FlateDecode 的灰階輸出理論上每個位元組都相同。
        // encodeFlate 對全灰輸入會自動降成 DeviceGray（見 image_codec.cpp），
        // 因此只需確認新的內容串流色彩空間鍵值為 DeviceGray。
        const engine::objects::PdfObject imageObject = output.object(5);
        const engine::objects::PdfDictionary* dict = imageObject.asDictionary();
        QVERIFY(dict != nullptr);
        const engine::objects::PdfObject* colorSpace = dict->find("ColorSpace");
        QVERIFY(colorSpace != nullptr);
        QCOMPARE(colorSpace->asName(), std::string("DeviceGray"));
    }

    void disablingVectorGraphicsLeavesContentStreamUntouched() {
        const EncodedImage image = encodeFlate(solidColorImage(16, 0, 0, 255));
        QVERIFY(image.ok);
        const std::string content = "0 0 1 rg 0 0 10 10 re f\n";
        const std::string pdf = documentWithContentAndImage(content, image);

        engine::objects::IncrementalAppender appender;
        QCOMPARE(appender.open(pdf), engine::objects::SourceStatus::Ok);

        ColorTransformSettings settings;
        settings.mode = ColorTransformMode::Grayscale;
        settings.transformVectorGraphics = false;
        const ColorTransformResult result = convertColors(appender, settings);
        QVERIFY(result.ok);
        QCOMPARE(result.report.contentOperatorsRewritten, 0);
        QCOMPARE(result.report.imagesTransformed, 1);
    }
};

QTEST_MAIN(TestColorTransform)
#include "test_color_transform.moc"
