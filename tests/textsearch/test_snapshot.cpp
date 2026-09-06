// 文件快照工具測試（PRD-TXT-005）：框選一塊區域複製為點陣影像。
//
// 驗證的重點不是「畫出來的像素好不好看」（那需要真正的向量內容與人眼比對），
// 是這個工具遵守了它自己宣稱的性質：只配置框選範圍大小的緩衝區（不是整頁），
// 框選範圍與頁面邊界相交後才是實際擷取到的範圍，以及退化輸入（範圍在頁面外、
// dpi 不合法）要回報失敗而不是安靜地給一張空圖。

#include <QtTest>

#include <string>

#include "engine/enhance/page_rasterizer.h"
#include "pdf_fixture.h"

using namespace alioth::engine::enhance;

namespace {
std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}
}  // namespace

class TestSnapshot : public QObject {
    Q_OBJECT

private slots:
    // makeSinglePagePdf()：200×400 點的頁面，左下角 100×200 的黑色實心矩形。
    void snapshotOnlyAllocatesTheRequestedRegion() {
        const std::string pdf = toStd(alioth::test::makeSinglePagePdf());
        SnapshotArea area{20.0, 20.0, 80.0, 80.0};  // 60×60 點的一小塊
        const SnapshotResult result = renderSnapshot(pdf, 0, area, 144.0, false);

        QVERIFY(result.ok);
        // 144 dpi、60 點 → 120 像素（60/72*144）。緩衝區必須剛好是這個大小，
        // 不是整頁（200/72*144 ≈ 400 像素寬）——這是「只渲染框選範圍」這條性質
        // 唯一能從輸出直接驗證的地方。
        QCOMPARE(result.pixels.width(), std::int32_t{120});
        QCOMPARE(result.pixels.height(), std::int32_t{120});
    }

    void snapshotClipsToPageBounds() {
        const std::string pdf = toStd(alioth::test::makeSinglePagePdf());
        // 頁面只有 200×400，框選範圍故意超出右邊與上邊。
        SnapshotArea area{150.0, -50.0, 400.0, 100.0};
        const SnapshotResult result = renderSnapshot(pdf, 0, area, 72.0, false);

        QVERIFY(result.ok);
        QCOMPARE(result.clippedArea.left, 150.0);
        QCOMPARE(result.clippedArea.right, 200.0);   // 頁寬上限
        QCOMPARE(result.clippedArea.top, 0.0);        // 下限裁到 0
        QCOMPARE(result.clippedArea.bottom, 100.0);
    }

    void snapshotCapturesBlackRectangleContent() {
        const std::string pdf = toStd(alioth::test::makeSinglePagePdf());
        // 黑色矩形在頁面座標的裝置空間裡是 x∈[0,100], y∈[200,400]（原點左上、
        // Y 向下的顯示空間；頁高 400，矩形畫在 PDF 空間 y∈[0,200]，
        // 顯示空間因此是 y∈[200,400]）。框住其中一小塊。
        SnapshotArea inside{10.0, 210.0, 40.0, 240.0};
        const SnapshotResult black = renderSnapshot(pdf, 0, inside, 72.0, false);
        QVERIFY(black.ok);

        // 框在矩形外的一塊，應該是白底。
        SnapshotArea outside{150.0, 10.0, 190.0, 40.0};
        const SnapshotResult white = renderSnapshot(pdf, 0, outside, 72.0, false);
        QVERIFY(white.ok);

        QVERIFY(!black.pixels.isNull());
        QVERIFY(!white.pixels.isNull());
        const std::uint8_t* blackPixel = black.pixels.data();
        const std::uint8_t* whitePixel = white.pixels.data();
        // BGRA：黑色矩形內應接近全 0，頁面空白處應接近全 255。
        QVERIFY(blackPixel[0] < 64);
        QVERIFY(whitePixel[0] > 200);
    }

    void invalidInputsFailCleanly() {
        const std::string pdf = toStd(alioth::test::makeSinglePagePdf());

        SnapshotResult badDpi = renderSnapshot(pdf, 0, SnapshotArea{0, 0, 10, 10}, 0.0, false);
        QVERIFY(!badDpi.ok);

        SnapshotResult badPage = renderSnapshot(pdf, 5, SnapshotArea{0, 0, 10, 10}, 72.0, false);
        QVERIFY(!badPage.ok);

        SnapshotResult outsideEntirely =
            renderSnapshot(pdf, 0, SnapshotArea{1000.0, 1000.0, 1100.0, 1100.0}, 72.0, false);
        QVERIFY(!outsideEntirely.ok);

        SnapshotResult emptyBytes = renderSnapshot("", 0, SnapshotArea{0, 0, 10, 10}, 72.0, false);
        QVERIFY(!emptyBytes.ok);
    }
};

QTEST_APPLESS_MAIN(TestSnapshot)
#include "test_snapshot.moc"
