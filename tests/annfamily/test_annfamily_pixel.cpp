// PRD-ANN-017 Highlight Area 的像素級驗收：以渲染回讀驗證真的變色，
// 且變色的方式確實是 Multiply 混合而不是普通透明疊色。
//
// 為什麼要挑灰色背景：白色背景下 Multiply(Cb,Cs) 與 Normal 混合的 blend(Cb,Cs)
// 在數學上會算出同一個結果（Multiply(1,Cs)=Cs），沒辦法從像素分辨兩者；
// 純紅背景則 G/B 分量已經是零，Multiply 後仍是零，同樣測不出差異。
// 只有中性灰背景能讓兩種混合模式算出可分辨、且與背景本身都不同的結果——
// 詳細算式見各測試案例內的註解。
//
// 渲染走 PdfiumEngine::renderThumbnail：那是 CLAUDE.md 明列的兩個允許整頁
// 光柵化的例外之一（縮圖／裁切偵測），離線、一次性、低解析度，符合條件。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "engine/cancellation.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/incremental_appender.h"
#include "engine/pdfium_engine.h"
#include "objects/object_fixture.h"

using namespace alioth;
using namespace alioth::domain;
using namespace alioth::engine::objects;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;

namespace {

struct Pixel {
    int r{0};
    int g{0};
    int b{0};
};

// 取樣點以頁面座標給定（原點左下、Y 向上），與 tests/pageops/pageops_render.h
// 的慣例一致；這裡另外寫一份而不是共用，是因為那個標頭與它的 fixture 綁在
// 同一個測試目錄，跨目錄相依只會讓兩邊將來各自的改動互相牽制。
Pixel samplePage(const engine::PixelBuffer& buffer, double pageWidth, double pageHeight,
                 double pageX, double pageY) {
    const double scaleX = static_cast<double>(buffer.width()) / pageWidth;
    const double scaleY = static_cast<double>(buffer.height()) / pageHeight;
    const int x = std::clamp(static_cast<int>(pageX * scaleX), 0, buffer.width() - 1);
    const int y = std::clamp(static_cast<int>((pageHeight - pageY) * scaleY), 0, buffer.height() - 1);
    // BGRA 預乘。stride 一律問 buffer，不可假設是寬×4（CLAUDE.md 硬性限制 3）。
    const std::uint8_t* row = buffer.data() + buffer.stride() * static_cast<std::size_t>(y);
    const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
    return Pixel{pixel[2], pixel[1], pixel[0]};
}

}  // namespace

class TestAnnfamilyPixel : public QObject {
    Q_OBJECT

private slots:
    void highlightAreaOverGreyBackgroundProvesRealMultiplyBlend() {
        // 1. 準備一份底色是中性灰的頁面：原始 fixture 的內容之外再蓋一層
        //    滿版灰色矩形，確保整頁背景可控，不受 fixture 預設內容影響。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const ContentAppendResult background =
            appendPageContent(appender, 0, "0.5 0.5 0.5 rg\n0 0 200 400 re\nf\n");
        QVERIFY2(background.ok, background.diagnostic.c_str());

        // 2. Highlight Area：直接拖一個矩形，不依賴文字層（PRD-ANN-017）。
        Annotation highlight;
        highlight.color = ColorRgb{1.0, 0.85, 0.0};
        highlight.opacity = 0.4;
        TextMarkupGeometry geometry;
        geometry.kind = TextMarkupKind::Highlight;
        geometry.quads.push_back(quadFromPageRect(RectF{50, 150, 150, 250}));
        highlight.geometry = geometry;

        const AnnotationWriteResult written = writeAnnotation(appender, 0, highlight);
        QVERIFY2(written.ok, written.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("highlight-area.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(built.bytes.data(), static_cast<qint64>(built.bytes.size()));
        file.close();

        // 3. 渲染整頁（縮圖路徑）並回讀像素。
        engine::PdfiumEngine engine;
        std::atomic<bool> opened{false};
        engine.openDocument(path.toStdString(), "",
                            [&opened](engine::OpenResult r) { opened = r.ok(); });
        engine.waitForIdle();
        QVERIFY(opened.load());

        engine::PixelBufferPtr buffer;
        engine.renderThumbnail(0, 400, engine::CancellationToken{},
                               [&buffer](engine::RenderResult r) {
                                   if (r.ok()) buffer = r.buffer;
                               });
        engine.waitForIdle();
        QVERIFY(buffer != nullptr);

        constexpr double kPageWidth = 200.0;
        constexpr double kPageHeight = 400.0;

        // 對照組：頁面上沒有螢光筆覆蓋的灰色區域，應該就是純灰 (128,128,128)。
        const Pixel control = samplePage(*buffer, kPageWidth, kPageHeight, 20.0, 380.0);
        QVERIFY2(std::abs(control.r - 128) <= 8 && std::abs(control.g - 128) <= 8 &&
                     std::abs(control.b - 128) <= 8,
                 qPrintable(QStringLiteral("背景取樣不是預期的灰色：(%1,%2,%3)")
                                .arg(control.r)
                                .arg(control.g)
                                .arg(control.b)));

        // 螢光筆覆蓋區域中心：取樣點刻意離邊界夠遠，避免抗鋸齒污染判定。
        const Pixel highlighted = samplePage(*buffer, kPageWidth, kPageHeight, 100.0, 200.0);

        // Multiply 混合的手算期望值（灰底 0.5、螢光筆黃色 1/0.85/0、不透明度 0.4）：
        //   result = (1-0.4)*backdrop + 0.4*(backdrop*source)
        //   R: 0.6*0.5 + 0.4*(0.5*1.0)  = 0.50 → 128
        //   G: 0.6*0.5 + 0.4*(0.5*0.85) = 0.47 → 120
        //   B: 0.6*0.5 + 0.4*(0.5*0.0)  = 0.30 →  77
        // 若混合模式被靜默降級成 Normal（blend(Cb,Cs)=Cs），R/G 會分別變成
        // 178/163——這正是本測試要攔住的那個 SDD §5.2 舊限制（ADR-002 之前）。
        QVERIFY2(std::abs(highlighted.r - 128) <= 12,
                 qPrintable(QStringLiteral("R 通道 = %1，預期約 128（Multiply）")
                                .arg(highlighted.r)));
        QVERIFY2(std::abs(highlighted.g - 120) <= 12,
                 qPrintable(QStringLiteral("G 通道 = %1，預期約 120（Multiply）；"
                                          "179 附近代表混合模式退化成了 Normal")
                                .arg(highlighted.g)));
        QVERIFY2(std::abs(highlighted.b - 77) <= 12,
                 qPrintable(QStringLiteral("B 通道 = %1，預期約 77").arg(highlighted.b)));

        // 明確排除「混合模式退化成 Normal」這個具體的錯誤結果，而不是只驗證
        // 「接近某個數字」——退化後的 R 通道會落在 179 附近，與 Multiply 預期的
        // 128 相距 51，遠超上面 12 的容忍度，兩者不會同時通過。
        QVERIFY(std::abs(highlighted.r - 179) > 20);

        // 真的變色：螢光筆區域必須與背景灰明顯不同（B 通道差距最大，128→77）。
        QVERIFY(std::abs(highlighted.b - control.b) >= 30);
    }
};

QTEST_APPLESS_MAIN(TestAnnfamilyPixel)
#include "test_annfamily_pixel.moc"
