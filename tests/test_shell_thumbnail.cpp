// 檔案總管縮圖渲染器（PRD-UI-020）。
//
// 這支程式碼會被載進 explorer.exe，因此測試的重點是**每一條失敗路徑都安靜地
// 失敗**：壞檔案、加密檔案、荒謬的尺寸要求，任何一個讓它崩潰的輸入都會把
// 使用者的檔案總管一起帶走。

#include <QtTest>

#include <QTemporaryDir>

#include "engine/shellthumb/thumbnail_renderer.h"
#include "pdf_fixture.h"

using namespace alioth;
using engine::shellthumb::renderFirstPageThumbnail;

class TestShellThumbnail : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        good_ = dir_->filePath(QStringLiteral("good.pdf"));
        QFile file(good_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(alioth::test::makeSinglePagePdf());
        file.close();
    }

    void rendersFirstPage() {
        const auto result = renderFirstPageThumbnail(good_.toStdString(), 128);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.width > 0);
        QVERIFY(result.height > 0);
        QCOMPARE(result.pixels.size(),
                 static_cast<std::size_t>(result.width) *
                     static_cast<std::size_t>(result.height) * 4u);
    }

    void longestEdgeMatchesTheRequest() {
        const auto result = renderFirstPageThumbnail(good_.toStdString(), 96);
        QVERIFY(result.ok);
        QCOMPARE(std::max(result.width, result.height), 96);
    }

    void aspectRatioIsPreserved() {
        // 拉伸的縮圖在檔案總管的方格裡一眼就看得出來不對。
        const auto small = renderFirstPageThumbnail(good_.toStdString(), 64);
        const auto large = renderFirstPageThumbnail(good_.toStdString(), 256);
        QVERIFY(small.ok && large.ok);
        const double smallRatio = static_cast<double>(small.width) / small.height;
        const double largeRatio = static_cast<double>(large.width) / large.height;
        QVERIFY2(std::abs(smallRatio - largeRatio) < 0.02,
                 qPrintable(QStringLiteral("長寬比 %1 vs %2").arg(smallRatio).arg(largeRatio)));
    }

    void backgroundIsOpaqueWhite() {
        // PDF 的頁面背景在規格上是透明的。不填白的話，深色主題的檔案總管
        // 會顯示一張看不見內容的圖。
        const auto result = renderFirstPageThumbnail(good_.toStdString(), 64);
        QVERIFY(result.ok);
        // 左上角通常是頁面邊界的留白。BGRA，alpha 在第四個位元組。
        QCOMPARE(result.pixels[3], static_cast<std::uint8_t>(255));
        QCOMPARE(result.pixels[0], static_cast<std::uint8_t>(255));
        QCOMPARE(result.pixels[1], static_cast<std::uint8_t>(255));
        QCOMPARE(result.pixels[2], static_cast<std::uint8_t>(255));
    }

    void missingFileFailsQuietly() {
        const auto result =
            renderFirstPageThumbnail(dir_->filePath(QStringLiteral("nope.pdf")).toStdString(), 64);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.pixels.empty());
    }

    void garbageFileFailsQuietly() {
        const QString path = dir_->filePath(QStringLiteral("garbage.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(4096, '\x01'));
        file.close();

        const auto result = renderFirstPageThumbnail(path.toStdString(), 64);
        QVERIFY(!result.ok);
        QVERIFY(result.pixels.empty());
    }

    void truncatedFileFailsQuietly() {
        const QString path = dir_->filePath(QStringLiteral("truncated.pdf"));
        const QByteArray full = alioth::test::makeSinglePagePdf();
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(full.left(full.size() / 3));
        file.close();

        const auto result = renderFirstPageThumbnail(path.toStdString(), 64);
        // 截斷的檔案 PDFium 有時修得回來、有時修不回來，兩種都可以接受；
        // 不可接受的是崩潰或回傳半套資料。
        if (result.ok) {
            QCOMPARE(result.pixels.size(),
                     static_cast<std::size_t>(result.width) *
                         static_cast<std::size_t>(result.height) * 4u);
        } else {
            QVERIFY(result.pixels.empty());
        }
    }

    void absurdSizesAreRejected() {
        // 一個被要求產生 4 萬像素縮圖的 Shell 擴充會把記憶體吃光，
        // 症狀是整個檔案總管沒有回應。
        QVERIFY(!renderFirstPageThumbnail(good_.toStdString(), 0).ok);
        QVERIFY(!renderFirstPageThumbnail(good_.toStdString(), -10).ok);
        QVERIFY(!renderFirstPageThumbnail(good_.toStdString(), 40000).ok);
    }

    void emptyPathIsRejected() {
        QVERIFY(!renderFirstPageThumbnail({}, 64).ok);
    }

    void repeatedCallsDoNotLeakHandles() {
        // 漏掉釋放的症狀是「開了幾百個資料夾之後檔案總管越來越慢」，
        // 那時已經很難追回是誰造成的。這裡只能驗它反覆呼叫仍然正確，
        // 真正的洩漏要靠 RAII 包裝本身保證。
        for (int i = 0; i < 20; ++i) {
            const auto result = renderFirstPageThumbnail(good_.toStdString(), 48);
            QVERIFY(result.ok);
        }
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString good_;
};

QTEST_MAIN(TestShellThumbnail)
#include "test_shell_thumbnail.moc"
