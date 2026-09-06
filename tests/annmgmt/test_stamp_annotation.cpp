// 圖章註解（PRD-ANN-006）。
//
// 兩種圖章的失敗方式不同，所以分開驗：
//   內建 — /Name 必須是規格的合法值（自創字串在別的檢視器上是空白方框），
//          外觀必須自己畫（PDFium 不替 /Stamp 產生 /AP，缺 /AP 就是一片空白）
//   自訂 — 影像要真的內嵌成 XObject 並在 /Resources 註冊；名稱對不上時
//          圖章一樣是空白，而且沒有任何錯誤訊息

#include <QtTest>

#include <QTemporaryDir>

#include "engine/annotations/appearance_stream.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"
#include "object_fixture.h"
#include "qpdf_check.h"

using namespace alioth;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;
using domain::StampGeometry;
using domain::StampKind;

namespace {

domain::Annotation builtIn(StampKind kind) {
    domain::Annotation annotation;
    StampGeometry stamp;
    stamp.kind = kind;
    annotation.geometry = stamp;
    annotation.rect = domain::RectF{100.0, 500.0, 260.0, 550.0};
    annotation.color = domain::stampDefaultColor(kind);
    annotation.author = "Reviewer";
    return annotation;
}

domain::Annotation customImage(int channels) {
    domain::Annotation annotation;
    StampGeometry stamp;
    stamp.kind = StampKind::Custom;
    stamp.imageWidth = 4;
    stamp.imageHeight = 3;
    stamp.imageChannels = channels;
    stamp.imagePixels.resize(static_cast<std::size_t>(4 * 3 * channels));
    for (std::size_t i = 0; i < stamp.imagePixels.size(); ++i) {
        stamp.imagePixels[i] = static_cast<std::uint8_t>(i * 7 % 256);
    }
    annotation.geometry = std::move(stamp);
    annotation.rect = domain::RectF{50.0, 400.0, 150.0, 475.0};
    return annotation;
}

}  // namespace

class TestStampAnnotation : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        base_ = toStdString(makeFixturePdf());
    }

    void builtInStampHasStandardName() {
        const domain::Annotation annotation = builtIn(StampKind::Approved);
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(base_) == engine::objects::SourceStatus::Ok);
        const auto written = engine::objects::writeAnnotation(appender, 0, annotation);
        QVERIFY2(written.ok, written.diagnostic.c_str());

        const auto built = appender.build();
        QVERIFY(built.ok);
        QVERIFY(built.bytes.find("/Subtype /Stamp") != std::string::npos ||
                built.bytes.find("/Subtype/Stamp") != std::string::npos);
        QVERIFY(built.bytes.find("/Approved") != std::string::npos);
    }

    void customStampDoesNotWriteAName() {
        // /Name 的值域是規格定死的。塞自訂字串進去會產出別人讀不懂的檔案。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(base_) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::objects::writeAnnotation(appender, 0, customImage(3)).ok);
        const auto built = appender.build();
        QVERIFY(built.ok);
        QVERIFY(built.bytes.find("/Name") == std::string::npos);
    }

    void everyBuiltInKindProducesAnAppearance() {
        // 缺 /AP 的圖章在多數檢視器上是一片空白——不是錯誤訊息，就只是空白。
        const StampKind kinds[] = {
            StampKind::Approved,     StampKind::Experimental,     StampKind::NotApproved,
            StampKind::AsIs,         StampKind::Expired,          StampKind::Draft,
            StampKind::Final,        StampKind::Confidential,     StampKind::ForComment,
            StampKind::TopSecret,    StampKind::ForPublicRelease, StampKind::NotForPublicRelease,
            StampKind::Sold,         StampKind::Departmental,
        };
        for (const StampKind kind : kinds) {
            const auto appearance = engine::annotations::generateAppearance(builtIn(kind));
            QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
            QVERIFY(!appearance.content.empty());
            // 圖章上要有字，而字需要 /Resources /Font。
            QVERIFY(appearance.needsFont);
            QVERIFY(!appearance.needsStampImage);
            QVERIFY(domain::stampNameOf(kind) != nullptr);
        }
    }

    void stampLabelIsDrawnInsideTheRect() {
        const auto appearance = engine::annotations::generateAppearance(builtIn(StampKind::Draft));
        QVERIFY(appearance.valid);
        QVERIFY(appearance.content.find("(DRAFT) Tj") != std::string::npos);
        // 外框與文字都要落在 /Rect 內，否則 Acrobat 會把超出的部分裁掉。
        QVERIFY(appearance.bbox.left >= 99.0);
        QVERIFY(appearance.bbox.right <= 261.0);
    }

    void longLabelIsScaledDownInsteadOfOverflowing() {
        // 「NOT FOR PUBLIC RELEASE」比「SOLD」長得多，同一個框裡字級必須不同，
        // 否則長的那個會滿出邊框。
        domain::Annotation shortLabel = builtIn(StampKind::Sold);
        domain::Annotation longLabel = builtIn(StampKind::NotForPublicRelease);
        const auto shortAppearance = engine::annotations::generateAppearance(shortLabel);
        const auto longAppearance = engine::annotations::generateAppearance(longLabel);
        QVERIFY(shortAppearance.valid && longAppearance.valid);
        QVERIFY(longAppearance.bbox.right <= longLabel.rect.right + 0.5);
    }

    void emptyRectIsRejectedWithAReason() {
        domain::Annotation annotation = builtIn(StampKind::Final);
        annotation.rect = domain::RectF{10.0, 10.0, 10.0, 10.0};
        const auto appearance = engine::annotations::generateAppearance(annotation);
        QVERIFY(!appearance.valid);
        QVERIFY(!appearance.diagnostic.empty());
    }

    void customStampWithoutImageFailsLoudly() {
        domain::Annotation annotation;
        StampGeometry stamp;
        stamp.kind = StampKind::Custom;
        annotation.geometry = stamp;
        annotation.rect = domain::RectF{10.0, 10.0, 60.0, 40.0};
        const auto appearance = engine::annotations::generateAppearance(annotation);
        QVERIFY(!appearance.valid);
        QVERIFY(!appearance.diagnostic.empty());
    }

    void mismatchedImageSizeIsRejected() {
        // 尺寸與位元組數不符的影像畫出來會是斜的，或直接讀到界外。
        domain::Annotation annotation = customImage(3);
        auto& stamp = std::get<StampGeometry>(annotation.geometry);
        stamp.imagePixels.pop_back();
        QVERIFY(!stamp.hasImage());
        const auto appearance = engine::annotations::generateAppearance(annotation);
        QVERIFY(!appearance.valid);
    }

    void customStampRegistersTheImageXObject() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(base_) == engine::objects::SourceStatus::Ok);
        const auto written = engine::objects::writeAnnotation(appender, 0, customImage(3));
        QVERIFY2(written.ok, written.diagnostic.c_str());

        const auto built = appender.build();
        QVERIFY(built.ok);
        // 內容串流引用 /Im0，資源字典必須有同名的項目——對不上時圖章是空白的，
        // 而且不會有任何錯誤。
        QVERIFY(built.bytes.find("/Im0 Do") != std::string::npos);
        QVERIFY(built.bytes.find("/XObject") != std::string::npos);
        // 序列化器是否在名稱之間留空白不是本測試的判準，兩種寫法都接受。
        QVERIFY(built.bytes.find("/Subtype /Image") != std::string::npos ||
                built.bytes.find("/Subtype/Image") != std::string::npos);
        QVERIFY(built.bytes.find("/DeviceRGB") != std::string::npos);
    }

    void rgbaStampSplitsAlphaIntoAnSMask() {
        // PDF 的影像沒有交錯式 alpha。RGBA 直接當 RGB 寫，每個像素多一個位元組，
        // 之後所有像素往後錯一格，整張圖顏色偏掉。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(base_) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::objects::writeAnnotation(appender, 0, customImage(4)).ok);
        const auto built = appender.build();
        QVERIFY(built.ok);
        QVERIFY(built.bytes.find("/SMask") != std::string::npos);
        QVERIFY(built.bytes.find("/DeviceGray") != std::string::npos);
    }

    void writeIsPurelyIncremental() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(base_) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::objects::writeAnnotation(appender, 0, builtIn(StampKind::Final)).ok);
        const auto built = appender.build();
        QVERIFY(built.ok);
        QCOMPARE(built.bytes.substr(0, base_.size()), base_);
    }

    void outputPassesQpdfStructureCheck() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(base_) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::objects::writeAnnotation(appender, 0, builtIn(StampKind::Confidential)).ok);
        QVERIFY(engine::objects::writeAnnotation(appender, 0, customImage(4)).ok);
        const auto built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = dir_->filePath(QStringLiteral("stamps.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
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
    std::string base_;
};

QTEST_MAIN(TestStampAnnotation)
#include "test_stamp_annotation.moc"
