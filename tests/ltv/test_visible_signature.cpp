// 可見簽章外觀（PRD-SIG-004 的補完，PRD-SIG-005 的前置條件，見 ADR-004）。
//
// 這一層畫的是給人看的東西，**沒有任何安全意義**——外觀可以偽造。因此本測試
// 除了驗幾何與資源接得上，也釘住一條規則：外觀上不得出現「已驗證」「有效」
// 這類字樣，那會讓一個未經驗證的檔案在畫面上宣稱自己是有效的。

#include <QtTest>

#include "engine/signature/signature_appearance.h"

using namespace alioth;
using engine::signature::AppearanceImage;
using engine::signature::SignatureAppearanceOptions;

namespace {

SignatureAppearanceOptions textOnly() {
    SignatureAppearanceOptions options;
    options.rectPt = domain::RectF{100.0, 500.0, 340.0, 560.0};
    options.signerName = "Eddy Chen";
    options.signingTime = "2026-09-06 14:32:05 +08:00";
    options.reason = "Design review";
    return options;
}

AppearanceImage sampleImage(int channels) {
    AppearanceImage image;
    image.width = 8;
    image.height = 4;
    image.channels = channels;
    image.pixels.assign(static_cast<std::size_t>(8 * 4 * channels), 90);
    return image;
}

}  // namespace

class TestVisibleSignature : public QObject {
    Q_OBJECT

private slots:
    void textOnlyAppearanceUsesTheFont() {
        const auto result = engine::signature::buildSignatureAppearance(textOnly());
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.needsFont);
        QVERIFY(!result.needsImage);
        QVERIFY(result.content.find("/Helv") != std::string::npos);
        QVERIFY(result.content.find("(Eddy Chen) Tj") != std::string::npos);
    }

    void bboxIsRelativeToItsOwnOrigin() {
        // 外觀的座標系原點在 /BBox 左下角，不是頁面座標。混淆兩者會讓外觀
        // 被畫到頁面的另一個角落。
        const auto result = engine::signature::buildSignatureAppearance(textOnly());
        QVERIFY(result.ok);
        QCOMPARE(result.bbox.left, 0.0);
        QCOMPARE(result.bbox.bottom, 0.0);
        QCOMPARE(result.bbox.width(), 240.0);
        QCOMPARE(result.bbox.height(), 60.0);
    }

    void appearanceNeverClaimsValidity() {
        // 外觀可以偽造。上面寫「已驗證」等於讓一個未經驗證的檔案自己宣稱有效。
        SignatureAppearanceOptions options = textOnly();
        options.reason = "Approved";
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(result.ok);
        const QString content = QString::fromStdString(result.content);
        QVERIFY(!content.contains(QStringLiteral("Verified"), Qt::CaseInsensitive));
        QVERIFY(!content.contains(QStringLiteral("Valid"), Qt::CaseInsensitive));
        QVERIFY(!content.contains(QStringLiteral("Trusted"), Qt::CaseInsensitive));
    }

    void nonAsciiFailsLoudlyInsteadOfDroppingCharacters() {
        // 靜默丟字會讓簽章欄顯示一個殘缺的姓名，而使用者不會發現。
        SignatureAppearanceOptions options = textOnly();
        options.signerName = "\xE9\x99\xB3\xE5\xB0\x8F\xE6\x98\x8E";  // 陳小明
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void imageAppearanceRegistersTheXObject() {
        SignatureAppearanceOptions options = textOnly();
        options.image = sampleImage(3);
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.needsImage);
        QVERIFY(result.content.find("/Im0 Do") != std::string::npos);
    }

    void imageKeepsItsAspectRatio() {
        // 拉伸簽名會讓筆跡變形，而那正是使用者用來辨認「這是我的簽名」的東西。
        SignatureAppearanceOptions options;
        options.rectPt = domain::RectF{0.0, 0.0, 400.0, 40.0};
        options.image = sampleImage(3);  // 8x4，長寬比 2:1
        options.signerName = "Eddy";
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(result.ok);

        // cm 矩陣的前兩個數字是縮放後的寬與高，比例必須維持 2:1。
        const QString content = QString::fromStdString(result.content);
        const int index = content.indexOf(QStringLiteral(" 0 0 "));
        QVERIFY(index > 0);
        const QStringList head =
            content.left(index).split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
        QVERIFY(!head.isEmpty());
        const double drawWidth = head.last().toDouble();
        const QStringList tail =
            content.mid(index + 5).split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts);
        QVERIFY(!tail.isEmpty());
        const double drawHeight = tail.first().toDouble();
        QVERIFY(drawWidth > 0.0 && drawHeight > 0.0);
        QVERIFY2(std::abs(drawWidth / drawHeight - 2.0) < 0.01,
                 qPrintable(QStringLiteral("長寬比變成 %1").arg(drawWidth / drawHeight)));
    }

    void rgbaImageIsAccepted() {
        SignatureAppearanceOptions options = textOnly();
        options.image = sampleImage(4);
        QVERIFY(options.image.isValid());
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(result.ok);
        QVERIFY(result.needsImage);
    }

    void mismatchedImageDataIsRejected() {
        SignatureAppearanceOptions options = textOnly();
        options.image = sampleImage(3);
        options.image.pixels.pop_back();
        QVERIFY(!options.image.isValid());
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(!result.ok);
    }

    void emptyRectIsRejected() {
        SignatureAppearanceOptions options = textOnly();
        options.rectPt = domain::RectF{10.0, 10.0, 10.0, 10.0};
        QVERIFY(!engine::signature::buildSignatureAppearance(options).ok);
    }

    void tinyRectFailsInsteadOfDrawingInvisibleText() {
        SignatureAppearanceOptions options = textOnly();
        options.rectPt = domain::RectF{0.0, 0.0, 240.0, 2.0};
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void nothingToDrawIsAnError() {
        SignatureAppearanceOptions options;
        options.rectPt = domain::RectF{0.0, 0.0, 100.0, 40.0};
        QVERIFY(!engine::signature::buildSignatureAppearance(options).ok);
    }

    void parenthesesInNamesAreEscapedOnce() {
        // 跳脫兩次會讓括號配對失衡，症狀是其後所有物件解析錯位，
        // 不是單一字串顯示怪異。
        SignatureAppearanceOptions options = textOnly();
        options.signerName = "Chen (Eddy)";
        const auto result = engine::signature::buildSignatureAppearance(options);
        QVERIFY(result.ok);
        QVERIFY(result.content.find("(Chen \\(Eddy\\)) Tj") != std::string::npos);
    }
};

QTEST_APPLESS_MAIN(TestVisibleSignature)
#include "test_visible_signature.moc"
