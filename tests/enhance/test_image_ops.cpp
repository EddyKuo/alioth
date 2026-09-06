// 掃描增強的像素演算法測試（WBS 14，PRD-ENH-002）。
//
// 這一支刻意不碰 PDF：去斜與增強的錯誤都是數值上的，而數值錯誤如果要透過
// 「生一份 PDF → 寫進去 → 讀回來 → 渲染」才看得到，失敗訊息會指向管線的
// 任何一段。合成影像進、數字出，失敗時就只有一個嫌疑犯。

#include <QtTest>

#include <cmath>

#include "domain/enhance.h"
#include "engine/enhance/image_ops.h"

using namespace alioth;
using namespace alioth::engine::enhance;
using domain::enhance::BinarizeMode;
using domain::enhance::DeskewResult;
using domain::enhance::DeskewSettings;
using domain::enhance::EnhanceSettings;

namespace {

void setPixel(engine::PixelBuffer& buffer, std::int32_t x, std::int32_t y, std::uint8_t gray) {
    std::uint8_t* row = buffer.scanline(y);
    row[x * 4 + 0] = gray;
    row[x * 4 + 1] = gray;
    row[x * 4 + 2] = gray;
    row[x * 4 + 3] = 255;
}

std::uint8_t pixelGray(const engine::PixelBuffer& buffer, std::int32_t x, std::int32_t y) {
    return buffer.data()[buffer.stride() * static_cast<std::size_t>(y) +
                         static_cast<std::size_t>(x) * 4];
}

// 一頁「文字」的替身：等間距的水平黑帶。真正的字形對投影剖面沒有意義——
// 它量的是行結構，而行結構就是這些帶。
engine::PixelBuffer syntheticTextPage(std::int32_t width = 480, std::int32_t height = 640,
                                      std::int32_t lineHeight = 6, std::int32_t lineGap = 18) {
    engine::PixelBuffer page = makeBuffer(width, height, 255);
    const std::int32_t marginX = width / 10;
    for (std::int32_t y = lineGap; y < height - lineGap; y += lineGap) {
        for (std::int32_t dy = 0; dy < lineHeight; ++dy) {
            const std::int32_t row = y + dy;
            if (row >= height) break;
            for (std::int32_t x = marginX; x < width - marginX; ++x) {
                setPixel(page, x, row, 0);
            }
        }
    }
    return page;
}

}  // namespace

class TestImageOps : public QObject {
    Q_OBJECT

private slots:
    void detectsKnownSkewAngle_data() {
        QTest::addColumn<double>("tilt");
        QTest::newRow("+1.0") << 1.0;
        QTest::newRow("+2.5") << 2.5;
        QTest::newRow("-3.0") << -3.0;
        QTest::newRow("+5.0") << 5.0;
    }

    void detectsKnownSkewAngle() {
        QFETCH(double, tilt);

        // 用同一個 rotate() 造出已知傾斜的影像，再要求偵測把角度找回來。
        // 這確保了偵測與校正的符號約定一致——符號寫反的話畫面上是
        // 「越轉越歪」，而角度的絕對值仍然對，單看數字不會發現。
        const engine::PixelBuffer tilted = rotate(syntheticTextPage(), tilt, 255);
        const DeskewResult result = detectSkew(tilted);

        QVERIFY2(result.detected, qPrintable(QString::fromStdString(result.note)));
        // 容許範圍取 0.4 度：合成影像的邊界效應（旋轉後角落補白）會讓
        // 最佳角度略微偏移，而 0.4 度的殘餘傾斜在 A4 寬度上不到 1.5 公釐。
        QVERIFY2(std::abs(result.angleDeg - tilt) < 0.4,
                 qPrintable(QStringLiteral("偵測 %1 度，實際 %2 度")
                                .arg(result.angleDeg).arg(tilt)));
        QCOMPARE(domain::enhance::correctionAngleDeg(result), -result.angleDeg);
    }

    void horizontalPageIsNotReportedAsSkewed() {
        // 誤判的代價比漏判高得多：正常的掃描件會被轉歪，而且是不可逆的
        // （重新取樣已經發生）。因此這條是本模組最重要的一條測試。
        const DeskewResult result = detectSkew(syntheticTextPage());
        QVERIFY2(!result.detected, qPrintable(QStringLiteral("水平頁被判定為傾斜 %1 度")
                                                  .arg(result.angleDeg)));
        QVERIFY(std::abs(result.angleDeg) < 0.15);
        QCOMPARE(domain::enhance::correctionAngleDeg(result), 0.0);
    }

    void blankPageIsNotReportedAsSkewed() {
        const DeskewResult result = detectSkew(makeBuffer(300, 300, 255));
        QVERIFY(!result.detected);
        QVERIFY(!result.note.empty());
    }

    void photographicPageIsRejectedRatherThanGuessed() {
        // 整頁雜訊沒有行結構。這種輸入正是投影剖面法的失效情境，
        // 而失效時必須回報「測不出來」，不是回報一個由雜訊決定的角度。
        engine::PixelBuffer noise = makeBuffer(320, 320, 255);
        std::uint32_t state = 12345u;
        for (std::int32_t y = 0; y < 320; ++y) {
            for (std::int32_t x = 0; x < 320; ++x) {
                state = state * 1664525u + 1013904223u;
                setPixel(noise, x, y, static_cast<std::uint8_t>((state >> 16) & 0xFF));
            }
        }
        const DeskewResult result = detectSkew(noise);
        QVERIFY2(!result.detected, qPrintable(QStringLiteral("雜訊頁被判定為傾斜 %1 度")
                                                  .arg(result.angleDeg)));
    }

    void rotationRestoresTiltedPage() {
        const engine::PixelBuffer original = syntheticTextPage();
        const engine::PixelBuffer tilted = rotate(original, 3.0, 255);
        const DeskewResult result = detectSkew(tilted);
        QVERIFY(result.detected);

        const engine::PixelBuffer fixed =
            rotate(tilted, domain::enhance::correctionAngleDeg(result), 255);
        const DeskewResult after = detectSkew(fixed);
        QVERIFY2(!after.detected, qPrintable(QStringLiteral("校正後仍判定傾斜 %1 度")
                                                 .arg(after.angleDeg)));
    }

    void toneCurveSaturatesInsteadOfWrapping() {
        // 255 + 10 若以 8 位元累加會變成 9，症狀是亮部出現黑點，
        // 看起來像雜訊而不像溢位。這條測試存在的唯一理由就是擋掉那個回繞。
        EnhanceSettings brighter;
        brighter.brightness = 10.0;
        QCOMPARE(static_cast<int>(domain::enhance::applyTone(255, brighter)), 255);
        QCOMPARE(static_cast<int>(domain::enhance::applyTone(250, brighter)), 255);

        EnhanceSettings darker;
        darker.brightness = -10.0;
        QCOMPARE(static_cast<int>(domain::enhance::applyTone(0, darker)), 0);
        QCOMPARE(static_cast<int>(domain::enhance::applyTone(5, darker)), 0);

        EnhanceSettings extreme;
        extreme.contrast = 100.0;
        extreme.brightness = 255.0;
        for (int i = 0; i < 256; ++i) {
            const int value = domain::enhance::applyTone(static_cast<std::uint8_t>(i), extreme);
            QVERIFY(value >= 0 && value <= 255);
        }

        EnhanceSettings extremeLow;
        extremeLow.contrast = 100.0;
        extremeLow.brightness = -255.0;
        for (int i = 0; i < 256; ++i) {
            const int value = domain::enhance::applyTone(static_cast<std::uint8_t>(i), extremeLow);
            QVERIFY(value >= 0 && value <= 255);
        }
    }

    void neutralSettingsLeavePixelsUnchanged() {
        // 「什麼都沒調」必須真的什麼都沒變。對比曲線在 c = 0 時若不是剛好等於 1，
        // 使用者每按一次確定畫質就掉一階，而單張圖看不出來。
        for (int i = 0; i < 256; ++i) {
            QCOMPARE(static_cast<int>(domain::enhance::applyTone(static_cast<std::uint8_t>(i), {})),
                     i);
        }
    }

    void contrastPivotsAroundMidGrey() {
        EnhanceSettings settings;
        settings.contrast = 50.0;
        // 中點不動，否則提高對比會同時整體變亮，使用者會以為亮度也被動過。
        QCOMPARE(static_cast<int>(domain::enhance::applyTone(128, settings)), 128);
        QVERIFY(domain::enhance::applyTone(200, settings) > 200);
        QVERIFY(domain::enhance::applyTone(60, settings) < 60);
    }

    void binarizeHandlesUniformAndGradientInputs() {
        EnhanceSettings otsu;
        otsu.binarize = BinarizeMode::Otsu;

        const engine::PixelBuffer black = applyEnhancement(makeBuffer(32, 32, 0), otsu);
        for (std::int32_t y = 0; y < 32; ++y) {
            for (std::int32_t x = 0; x < 32; ++x) {
                QCOMPARE(static_cast<int>(pixelGray(black, x, y)), 0);
            }
        }

        const engine::PixelBuffer white = applyEnhancement(makeBuffer(32, 32, 255), otsu);
        for (std::int32_t y = 0; y < 32; ++y) {
            for (std::int32_t x = 0; x < 32; ++x) {
                QCOMPARE(static_cast<int>(pixelGray(white, x, y)), 255);
            }
        }

        engine::PixelBuffer gradient = makeBuffer(256, 8, 0);
        for (std::int32_t y = 0; y < 8; ++y) {
            for (std::int32_t x = 0; x < 256; ++x) {
                setPixel(gradient, x, y, static_cast<std::uint8_t>(x));
            }
        }

        EnhanceSettings fixed;
        fixed.binarize = BinarizeMode::Fixed;
        fixed.fixedThreshold = 128;
        const engine::PixelBuffer split = applyEnhancement(gradient, fixed);
        QCOMPARE(static_cast<int>(pixelGray(split, 0, 0)), 0);
        QCOMPARE(static_cast<int>(pixelGray(split, 127, 0)), 0);
        QCOMPARE(static_cast<int>(pixelGray(split, 128, 0)), 255);
        QCOMPARE(static_cast<int>(pixelGray(split, 255, 0)), 255);

        // 二值化的輸出只允許兩個值。中間值代表門檻沒有真的被套用，
        // 而那在縮圖上完全看不出來。
        const engine::PixelBuffer otsuGradient = applyEnhancement(gradient, otsu);
        for (std::int32_t x = 0; x < 256; ++x) {
            const int value = pixelGray(otsuGradient, x, 0);
            QVERIFY(value == 0 || value == 255);
        }
    }

    void otsuThresholdSplitsBimodalHistogram() {
        std::array<std::uint64_t, 256> histogram{};
        histogram[30] = 1000;
        histogram[220] = 1000;
        const std::uint8_t threshold = domain::enhance::otsuThreshold(histogram);
        QVERIFY(threshold > 30 && threshold <= 220);

        std::array<std::uint64_t, 256> empty{};
        QCOMPARE(static_cast<int>(domain::enhance::otsuThreshold(empty)), 128);
    }

    void backgroundPlacementRespectsFitAndAnchor() {
        const domain::SizeF page{200.0, 100.0};
        const domain::SizeF image{100.0, 100.0};  // 正方形放進長方形頁面

        domain::enhance::BackgroundSpec stretch;
        stretch.source = domain::enhance::BackgroundSource::Image;
        stretch.imageBytes.push_back(0);  // valid() 需要非空，內容在此無關
        stretch.fit = domain::enhance::BackgroundFit::Stretch;
        const domain::RectF filled = domain::enhance::backgroundPlacement(page, image, stretch);
        QCOMPARE(filled.width(), 200.0);
        QCOMPARE(filled.height(), 100.0);

        domain::enhance::BackgroundSpec fit = stretch;
        fit.fit = domain::enhance::BackgroundFit::Fit;
        const domain::RectF fitted = domain::enhance::backgroundPlacement(page, image, fit);
        QCOMPARE(fitted.width(), 100.0);
        QCOMPARE(fitted.height(), 100.0);
        QCOMPARE(fitted.left, 50.0);  // 預設置中

        domain::enhance::BackgroundSpec fill = stretch;
        fill.fit = domain::enhance::BackgroundFit::Fill;
        const domain::RectF filledOut = domain::enhance::backgroundPlacement(page, image, fill);
        QCOMPARE(filledOut.width(), 200.0);
        QCOMPARE(filledOut.height(), 200.0);  // 覆蓋整頁，垂直方向溢出

        domain::enhance::BackgroundSpec corner = fit;
        corner.anchor = domain::enhance::BackgroundAnchor::BottomLeft;
        const domain::RectF anchored = domain::enhance::backgroundPlacement(page, image, corner);
        QCOMPARE(anchored.left, 0.0);
        QCOMPARE(anchored.bottom, 0.0);
    }

    void backgroundPlacementHonoursMargins() {
        const domain::SizeF page{200.0, 200.0};
        domain::enhance::BackgroundSpec spec;  // 純色，撐滿留白後的框
        spec.marginLeft = 10.0;
        spec.marginBottom = 20.0;
        spec.marginRight = 30.0;
        spec.marginTop = 40.0;
        const domain::RectF rect = domain::enhance::backgroundPlacement(page, {}, spec);
        QCOMPARE(rect.left, 10.0);
        QCOMPARE(rect.bottom, 20.0);
        QCOMPARE(rect.right, 170.0);
        QCOMPARE(rect.top, 160.0);

        // 留白大到把框吃光時必須回傳空矩形，而不是負寬高的矩形——
        // 負寬高會讓之後的每一次縮放把圖翻面。
        domain::enhance::BackgroundSpec impossible;
        impossible.marginLeft = 150.0;
        impossible.marginRight = 150.0;
        QVERIFY(domain::enhance::backgroundPlacement(page, {}, impossible).isEmpty());
    }

    void grayscaleUsesLuminanceWeights() {
        engine::PixelBuffer colored = makeBuffer(2, 1, 0);
        // BGRA：純藍與純綠的亮度必須不同，否則有色印章與螢光筆會被混為一談。
        std::uint8_t* row = colored.scanline(0);
        row[0] = 255; row[1] = 0; row[2] = 0; row[3] = 255;   // 藍
        row[4] = 0; row[5] = 255; row[6] = 0; row[7] = 255;   // 綠

        EnhanceSettings settings;
        settings.grayscale = true;
        const engine::PixelBuffer gray = applyEnhancement(colored, settings);
        QVERIFY(pixelGray(gray, 0, 0) < pixelGray(gray, 1, 0));
    }
};

QTEST_APPLESS_MAIN(TestImageOps)
#include "test_image_ops.moc"
