// 列印端到端：PrintService → QPrinter → PDF 檔（PRD-IO-008）。
//
// CI 上沒有印表機，所以用 QPrinter::PdfFormat 當替身：它走的是同一條
// QPainter 路徑、同一套分頁與座標換算，唯一不同的是輸出落到檔案而不是紙。
// 產出的 PDF 再用 PdfiumEngine 開回來，就能斷言張數確實正確。

#include <QtTest>

#include <QFileInfo>
#include <QPrinter>
#include <QTemporaryDir>

#include <future>

#include "app/print/print_service.h"
#include "engine/pdfium_engine.h"
#include "print_fixture.h"

using namespace alioth;
using namespace alioth::app::print;

namespace {

// 開回產出的 PDF 並回報頁數。-1 表示開不起來——那本身就是有效的失敗訊號，
// 因為列印產物必須是結構合法的 PDF。
int pageCountOf(const QString& path) {
    engine::PdfiumEngine engine;
    std::promise<engine::OpenResult> promise;
    auto future = promise.get_future();
    engine.openDocument(path.toStdString(), {},
                        [&promise](engine::OpenResult result) { promise.set_value(result); });
    const engine::OpenResult result = future.get();
    return result.ok() ? result.info.pageCount : -1;
}

// 把產出的 PDF 的第一頁整頁縮成點陣，回傳深色像素的重心（0..1 的相對座標，
// 原點左上）。用來確認紙上真的有內容、而且落在該落的位置——「印出一疊白紙」
// 是列印最典型的無聲失敗。
bool darkPixelCentroid(const QString& path, QPointF* centroid) {
    engine::PdfiumEngine engine;
    std::promise<engine::OpenResult> openPromise;
    auto openFuture = openPromise.get_future();
    engine.openDocument(path.toStdString(), {},
                        [&openPromise](engine::OpenResult result) { openPromise.set_value(result); });
    if (!openFuture.get().ok()) return false;

    std::promise<engine::RenderResult> renderPromise;
    auto renderFuture = renderPromise.get_future();
    engine.renderThumbnail(0, 400, engine::CancellationToken{},
                           [&renderPromise](engine::RenderResult result) {
                               renderPromise.set_value(std::move(result));
                           });
    const engine::RenderResult rendered = renderFuture.get();
    if (!rendered.ok()) return false;

    const engine::PixelBuffer& buffer = *rendered.buffer;
    double sumX = 0.0;
    double sumY = 0.0;
    long long count = 0;
    for (std::int32_t y = 0; y < buffer.height(); ++y) {
        const std::uint8_t* row = buffer.data() + buffer.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < buffer.width(); ++x) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            if (px[0] < 128 && px[1] < 128 && px[2] < 128) {
                sumX += x;
                sumY += y;
                ++count;
            }
        }
    }
    if (count == 0) return false;
    *centroid = QPointF{sumX / count / buffer.width(), sumY / count / buffer.height()};
    return true;
}

void configurePdfPrinter(QPrinter& printer, const QString& path) {
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageOrientation(QPageLayout::Portrait);
    printer.setPageMargins(QMarginsF(10, 10, 10, 10), QPageLayout::Millimeter);
}

}  // namespace

class TestPrintOutput : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        file_ = test::writeTempPdfFile(test::makeMultiPagePdf(4));
        QVERIFY(file_ != nullptr);

        QString error;
        QVERIFY2(service_.openDocument(file_->fileName(), {}, &error),
                 qPrintable(QStringLiteral("開檔失敗：%1").arg(error)));
        QCOMPARE(service_.pageCount(), 4);
    }

    void pageSizeComesFromTheEngineNotFromAGuess() {
        QCOMPARE(service_.pageSizePt(0), QSizeF(200.0, 400.0));
        QCOMPARE(service_.pageSizePt(99), QSizeF());
    }

    void printableRectIsAnchoredAtOrigin() {
        QPrinter printer;
        configurePdfPrinter(printer, output(QStringLiteral("unused.pdf")));
        const QRectF rect = PrintService::printableRectPt(printer);
        // 原點必須歸零：QPainter 在 QPrinter 上的裝置原點就是可列印區左上角，
        // 若沿用 QPageLayout 回報的邊距原點，所有內容會多偏移一個邊距。
        QCOMPARE(rect.topLeft(), QPointF(0.0, 0.0));
        QVERIFY(rect.width() > 400.0);
        QVERIFY(rect.height() > 600.0);
    }

    void printsEveryPageByDefault() {
        const QString path = output(QStringLiteral("all.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;

        const PrintResult result = service_.print(printer, options);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.sheetsPrinted, 4);
        verifyOutput(path, 4);
    }

    void pageRangeLimitsSheetCount() {
        const QString path = output(QStringLiteral("range.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.pageRangeSpec = QStringLiteral("2-3");

        const PrintResult result = service_.print(printer, options);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.sheetsPrinted, 2);
        verifyOutput(path, 2);
    }

    void posterSplitProducesOneSheetPerTile() {
        const QString path = output(QStringLiteral("poster.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.pageRangeSpec = QStringLiteral("1");
        options.poster.enabled = true;
        options.poster.zoomPercent = 200.0;
        options.poster.overlapPt = 0.0;

        const PrintPlan plan = service_.planFor(printer, options);
        QVERIFY(plan.valid);
        QCOMPARE(plan.sheetCount(), plan.sheets[0].posterColumns * plan.sheets[0].posterRows);

        const PrintResult result = service_.print(printer, options);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.sheetsPrinted, plan.sheetCount());
        verifyOutput(path, plan.sheetCount());
    }

    void bothAnnotationModesComplete() {
        for (const bool withAnnotations : {true, false}) {
            const QString path =
                output(withAnnotations ? QStringLiteral("annots-on.pdf")
                                       : QStringLiteral("annots-off.pdf"));
            QPrinter printer;
            configurePdfPrinter(printer, path);

            PrintOptions options;
            options.renderDpi = kTestDpi;
            options.includeAnnotations = withAnnotations;
            options.pageRangeSpec = QStringLiteral("1-2");

            const PrintResult result = service_.print(printer, options);
            QVERIFY2(result.ok, qPrintable(result.message));
            verifyOutput(path, 2);
        }
    }

    void duplexAndCopiesAreAcceptedByThePdfBackend() {
        const QString path = output(QStringLiteral("duplex.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.duplex = DuplexMode::LongSide;
        options.copies = 2;
        options.pageRangeSpec = QStringLiteral("1");

        const PrintResult result = service_.print(printer, options);
        QVERIFY2(result.ok, qPrintable(result.message));
        // 份數由驅動端複製，計畫仍然只有一張。PDF 後端不會真的複製頁面。
        QCOMPARE(result.sheetsPrinted, 1);
        verifyOutput(path, 1);
    }

    void stampsAndBatesAreEmittedForEverySheet() {
        const QString path = output(QStringLiteral("stamped.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.stamps.bates.enabled = true;
        options.stamps.bates.prefix = QStringLiteral("ALIOTH-");
        options.stamps.bates.digits = 5;
        options.stamps.bates.startNumber = 1;
        options.stamps.headerFooter.push_back(StampText{
            QStringLiteral("<<FileName>> — <<Page>>/<<Pages>>"), StampAnchor::TopCenter, 9.0,
            QString{}, false, QColor(0, 0, 0), 1.0, 0.0});
        options.stamps.watermark.enabled = true;
        options.stamps.watermark.text = QStringLiteral("CONFIDENTIAL");

        const PrintResult result = service_.print(printer, options);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.batesNumbers.size(), std::size_t{4});
        QCOMPARE(result.batesNumbers.front(), QStringLiteral("ALIOTH-00001"));
        QCOMPARE(result.batesNumbers.back(), QStringLiteral("ALIOTH-00004"));
        verifyOutput(path, 4);
    }

    void duplicateBatesSequenceIsRefusedBeforePrinting() {
        const QString path = output(QStringLiteral("dupe.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.stamps.bates.enabled = true;
        options.stamps.bates.increment = 0;

        const PrintResult result = service_.print(printer, options);
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());
        // 擋下來就不該留下半份輸出。
        QVERIFY(!QFileInfo::exists(path) || QFileInfo(path).size() == 0);
    }

    void higherDpiProducesLargerOutputThanLowDpi() {
        // 列印品質的可觀察代理指標：同一頁在較高 dpi 下重新渲染，內嵌影像
        // 的像素量增加，檔案必然變大。若列印沿用螢幕圖磚再放大，
        // 兩者的位元組數會相同——那正是這個測試要擋下的退化。
        const QString lowPath = output(QStringLiteral("dpi-low.pdf"));
        const QString highPath = output(QStringLiteral("dpi-high.pdf"));

        PrintOptions options;
        options.pageRangeSpec = QStringLiteral("1");

        QPrinter lowPrinter;
        configurePdfPrinter(lowPrinter, lowPath);
        options.renderDpi = 96;
        QVERIFY(service_.print(lowPrinter, options).ok);

        QPrinter highPrinter;
        configurePdfPrinter(highPrinter, highPath);
        options.renderDpi = 300;
        QVERIFY(service_.print(highPrinter, options).ok);

        QVERIFY(QFileInfo(highPath).size() > QFileInfo(lowPath).size());
    }

    void printedSheetActuallyContainsThePageContent() {
        const QString path = output(QStringLiteral("content.pdf"));
        QPrinter printer;
        configurePdfPrinter(printer, path);

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.pageRangeSpec = QStringLiteral("1");

        QVERIFY(service_.print(printer, options).ok);

        QPointF centroid;
        QVERIFY2(darkPixelCentroid(path, &centroid), "印出來的是一張白紙");
        // fixture 在頁面左下角畫實心方塊；符合紙張時整頁等比放大置中，
        // 方塊仍應留在左下象限。座標弄反（Y 軸沒翻）會讓它跑到左上。
        QVERIFY2(centroid.x() < 0.5, qPrintable(QString::number(centroid.x())));
        QVERIFY2(centroid.y() > 0.5, qPrintable(QString::number(centroid.y())));
    }

    void posterFirstSheetShowsTheTopLeftPartOnly() {
        // 海報分割最容易出的錯是每張紙都印同一塊內容（切片原點沒套用）。
        // 那種錯誤下，第一張紙的內容會與整頁列印完全相同，因此拿兩者的
        // 深色像素重心來比對就足以識別。
        const QString wholePath = output(QStringLiteral("poster-whole.pdf"));
        const QString posterPath = output(QStringLiteral("poster-content.pdf"));

        PrintOptions options;
        options.renderDpi = kTestDpi;
        options.pageRangeSpec = QStringLiteral("1");

        QPrinter wholePrinter;
        configurePdfPrinter(wholePrinter, wholePath);
        QVERIFY(service_.print(wholePrinter, options).ok);

        options.poster.enabled = true;
        options.poster.zoomPercent = 200.0;
        options.poster.overlapPt = 0.0;
        QPrinter posterPrinter;
        configurePdfPrinter(posterPrinter, posterPath);
        QVERIFY(service_.print(posterPrinter, options).ok);

        QPointF whole;
        QPointF poster;
        QVERIFY(darkPixelCentroid(wholePath, &whole));
        QVERIFY2(darkPixelCentroid(posterPath, &poster), "海報第一張紙是空白的");
        // 實心方塊在頁面下半部，放大兩倍後落在第三、四張紙上，
        // 所以第一張紙的重心必然明顯往上。
        QVERIFY2(poster.y() < whole.y() - 0.15,
                 qPrintable(QStringLiteral("whole=%1 poster=%2").arg(whole.y()).arg(poster.y())));
    }

    void printingWithoutDocumentFailsCleanly() {
        PrintService empty;
        QPrinter printer;
        configurePdfPrinter(printer, output(QStringLiteral("empty.pdf")));
        const PrintResult result = empty.print(printer, PrintOptions{});
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());
    }

private:
    // 測試一律用 150 dpi：足以讓「以印表機解析度重新渲染」這條路徑真的被走到，
    // 又不會讓每個案例都花上數秒渲染上百塊圖磚。
    static constexpr int kTestDpi = 150;

    QString output(const QString& name) const { return dir_.filePath(name); }

    void verifyOutput(const QString& path, int expectedPages) {
        const QFileInfo info(path);
        QVERIFY2(info.exists(), qPrintable(path));
        QVERIFY(info.size() > 0);
        QCOMPARE(pageCountOf(path), expectedPages);
    }

    QTemporaryDir dir_;
    std::unique_ptr<QTemporaryFile> file_;
    PrintService service_;
};

QTEST_MAIN(TestPrintOutput)
#include "test_print_output.moc"
