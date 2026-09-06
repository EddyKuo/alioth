// 裁切測試（WBS 5.8，PRD-PAGE-003）。
//
// 兩個不可退讓的性質：CropBox 真的寫進文件並且重新開啟後還在，以及 MediaBox
// 完全沒被動過。後者是可還原性的基礎——裁切在 PDF 裡是「只顯示這一塊」，
// 不是「把內容切掉」，MediaBox 一旦被改寫，原本的頁面尺寸就找不回來了。

#include <QtTest>

#include <optional>
#include <vector>

#include "engine/pages/page_editor.h"
#include "page_fixture.h"

using namespace alioth::domain;
using namespace alioth::engine::pages;

namespace {

bool nearly(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

}  // namespace

class TestPageCrop : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        source_ = dir_->filePath(QStringLiteral("inked.pdf"));
        target_ = dir_->filePath(QStringLiteral("cropped.pdf"));
        QVERIFY(alioth::test::writePdfTo(source_, alioth::test::makeInkedPdf()));
    }

    void cleanup() { dir_.reset(); }

    void reportsMissingCropBoxAsUnset() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // 語料沒有 /CropBox。「未設定」與「等於 MediaBox」在檔案裡是兩件事，
        // 不能替使用者補上一個看起來一樣的值。
        QVERIFY(!editor.cropBox(0).has_value());
        const auto media = editor.mediaBox(0);
        QVERIFY(media.has_value());
        QCOMPARE(media->width(), alioth::test::kInkedPageWidth);
        QCOMPARE(media->height(), alioth::test::kInkedPageHeight);
    }

    void setCropBoxSurvivesRoundTrip() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        const RectF box{50.0, 60.0, 250.0, 300.0};
        QVERIFY(editor.setCropBox(0, box).ok());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        PageEditor reopened;
        QVERIFY(reopened.open(target_.toStdString()));
        const auto crop = reopened.cropBox(0);
        QVERIFY(crop.has_value());
        QCOMPARE(crop->left, box.left);
        QCOMPARE(crop->bottom, box.bottom);
        QCOMPARE(crop->right, box.right);
        QCOMPARE(crop->top, box.top);

        // MediaBox 未被破壞。
        const auto media = reopened.mediaBox(0);
        QVERIFY(media.has_value());
        QCOMPARE(media->left, 0.0);
        QCOMPARE(media->bottom, 0.0);
        QCOMPARE(media->right, alioth::test::kInkedPageWidth);
        QCOMPARE(media->top, alioth::test::kInkedPageHeight);

        // 裁切生效的可觀察結果：PDFium 算出來的頁面尺寸變成裁切框的大小。
        const auto size = reopened.pageSize(0);
        QVERIFY(size.has_value());
        QCOMPARE(size->width, box.width());
        QCOMPARE(size->height, box.height());

        // 未被裁切的第二頁不受影響。
        QVERIFY(!reopened.cropBox(1).has_value());
    }

    void cropBoxIsClampedToMediaBox() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // 超出 MediaBox 的裁切框在各家檢視器的行為並不一致，先夾好再寫。
        QVERIFY(editor.setCropBox(0, RectF{-50.0, -50.0, 900.0, 900.0}).ok());
        const auto crop = editor.cropBox(0);
        QVERIFY(crop.has_value());
        QCOMPARE(crop->left, 0.0);
        QCOMPARE(crop->bottom, 0.0);
        QCOMPARE(crop->right, alioth::test::kInkedPageWidth);
        QCOMPARE(crop->top, alioth::test::kInkedPageHeight);
    }

    void setCropBoxRejectsInvalidInput() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QCOMPARE(editor.setCropBox(0, RectF{100.0, 100.0, 100.0, 100.0}).status,
                 PageEditStatus::InvalidArgument);
        QCOMPARE(editor.setCropBox(9, RectF{0.0, 0.0, 10.0, 10.0}).status,
                 PageEditStatus::InvalidArgument);
        // 完全落在 MediaBox 之外的框夾完會變成空的，屬於不合法而不是「夾成一條線」。
        QCOMPARE(editor.setCropBox(0, RectF{500.0, 600.0, 700.0, 800.0}).status,
                 PageEditStatus::InvalidArgument);
    }

    // ---- 裁切至白邊 -------------------------------------------------------

    void detectsContentBounds() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        const auto bounds = editor.detectContentBounds(0);
        QVERIFY(bounds.has_value());

        // 偵測精度就是一個像素對應的點數（頁寬 / maxEdgePixels），這裡約 0.5 點。
        // 容忍值放到 2 點，避免抗鋸齒讓測試變得脆弱。
        constexpr double kTolerance = 2.0;
        QVERIFY(nearly(bounds->left, alioth::test::kInkedRectLeft, kTolerance));
        QVERIFY(nearly(bounds->bottom, alioth::test::kInkedRectBottom, kTolerance));
        QVERIFY(nearly(bounds->right, alioth::test::kInkedRectRight, kTolerance));
        QVERIFY(nearly(bounds->top, alioth::test::kInkedRectTop, kTolerance));
    }

    void blankPageHasNoContentBounds() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // 整頁皆白時回傳 nullopt：把空矩形拿去裁切只會產生壞檔案。
        QVERIFY(!editor.detectContentBounds(1).has_value());
    }

    void cropToContentSkipsBlankPages() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        ContentBoundsOptions options;
        options.marginPt = 5.0;

        int cropped = 0;
        QVERIFY(editor.cropToContent({0, 1}, options, &cropped).ok());
        QCOMPARE(cropped, 1);
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        PageEditor reopened;
        QVERIFY(reopened.open(target_.toStdString()));
        const auto crop = reopened.cropBox(0);
        QVERIFY(crop.has_value());
        // 邊界含 5 點的安全邊界，內容一定完整落在框內。
        QVERIFY(crop->left < alioth::test::kInkedRectLeft);
        QVERIFY(crop->bottom < alioth::test::kInkedRectBottom);
        QVERIFY(crop->right > alioth::test::kInkedRectRight);
        QVERIFY(crop->top > alioth::test::kInkedRectTop);
        // 空白頁沒有被裁切，也沒有被寫進一個零面積的框。
        QVERIFY(!reopened.cropBox(1).has_value());

        const auto media = reopened.mediaBox(0);
        QVERIFY(media.has_value());
        QCOMPARE(media->right, alioth::test::kInkedPageWidth);
        QCOMPARE(media->top, alioth::test::kInkedPageHeight);
    }

    void cropToContentRejectsInvalidSelection() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QCOMPARE(editor.cropToContent({}).status, PageEditStatus::InvalidArgument);
        QCOMPARE(editor.cropToContent({7}).status, PageEditStatus::InvalidArgument);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString source_;
    QString target_;
};

QTEST_APPLESS_MAIN(TestPageCrop)
#include "test_page_crop.moc"
