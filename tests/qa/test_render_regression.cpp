// 渲染回歸的基準影像框架（WBS 7.3、PRD §9）。
//
// 這道關卡要抓的是「沒有人動過渲染程式碼，但畫面變了」——PDFium 每季升版、
// 抗鋸齒或字型後備行為改變、我們自己動到零複製路徑的 stride 或矩陣。
// 這些改變不會讓任何既有測試變紅：座標往返還是對的、圖磚還是有像素、
// 快取還是會命中。只有把畫面存下來逐張比對才看得見。
//
// 基準影像存在 tests/golden/render/。第一次執行（或新增情境）時基準不存在，
// 框架會產生基準並明確標示「已建立基準，本次未比對」而不是靜默通過——
// 一個什麼都沒比對卻顯示綠燈的測試，比沒有這個測試更危險。
//
// 比對指標的選擇與門檻理由見 golden_image.h。

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <memory>
#include <string>

#include "domain/tile.h"
#include "engine/pdfium_engine.h"
#include "engine/pixel_buffer.h"
#include "engineering_corpus.h"
#include "golden_image.h"

using namespace alioth;

namespace {

// 情境定義。每個情境是「語料 + 頁 + 倍率 + 渲染選項」的一組固定值。
// 改動任何一項都會讓基準失效，因此全部集中在這張表上，不散落在各測試函式裡。
struct Scenario {
    const char* name;
    std::int32_t pageIndex;
    double scale;
    engine::RenderOptions options;
};

// PixelBuffer 是 BGRA 預乘，與 QImage::Format_ARGB32_Premultiplied 在小端序機器上
// 記憶體佈局相同，因此可以零複製包裝。stride 一律取自緩衝區而不是「寬 × 4」——
// 假設寬 × 4 產生的是斜切畫面而不是崩潰（CLAUDE.md 硬性限制 3）。
QImage wrapTile(const engine::PixelBuffer& buffer) {
    return QImage(buffer.data(), buffer.width(), buffer.height(),
                  static_cast<qsizetype>(buffer.stride()),
                  QImage::Format_ARGB32_Premultiplied)
        .copy();
}

// 把整頁以圖磚拼出來。
//
// 刻意不呼叫 renderThumbnail（那是整頁光柵化的合法例外之一）：本測試要守的是
// 正式的圖磚路徑，包含分塊的邊界。少畫一條在圖磚接縫上的線，只有走圖磚路徑才看得到。
QImage renderPageByTiles(engine::PdfiumEngine& engine, const domain::PageInfo& info, double scale,
                         const engine::RenderOptions& options) {
    const int widthPx = static_cast<int>(std::lround(info.sizePt.width * scale));
    const int heightPx = static_cast<int>(std::lround(info.sizePt.height * scale));
    if (widthPx <= 0 || heightPx <= 0) return {};

    QImage page(widthPx, heightPx, QImage::Format_ARGB32_Premultiplied);
    page.fill(Qt::white);

    const int columns = (widthPx + domain::kTileSize - 1) / domain::kTileSize;
    const int rows = (heightPx + domain::kTileSize - 1) / domain::kTileSize;

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const domain::TileKey key{info.index,          domain::exactScaleKey(scale),
                                      column,              row,
                                      domain::Rotation::None, options.nightMode};
            QImage tile;
            engine.renderTile(key, options, domain::TaskPriority::Visible, engine::CancellationToken{},
                              [&tile](engine::RenderResult result) {
                                  if (result.ok()) tile = wrapTile(*result.buffer);
                              });
            engine.waitForIdle();
            if (tile.isNull()) continue;

            // 圖磚是固定 512×512，頁面右下角的圖磚會超出頁面，多出來的部分丟掉。
            const int copyWidth = std::min(domain::kTileSize, widthPx - column * domain::kTileSize);
            const int copyHeight = std::min(domain::kTileSize, heightPx - row * domain::kTileSize);
            for (int y = 0; y < copyHeight; ++y) {
                const auto* src = reinterpret_cast<const QRgb*>(tile.constScanLine(y));
                auto* dst = reinterpret_cast<QRgb*>(page.scanLine(row * domain::kTileSize + y));
                for (int x = 0; x < copyWidth; ++x) {
                    dst[column * domain::kTileSize + x] = src[x];
                }
            }
        }
    }
    return page;
}

}  // namespace

class TestRenderRegression : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        goldenDir_ = QStringLiteral(ALIOTH_GOLDEN_DIR) + QStringLiteral("/render");
        // 失敗產物（實際輸出與差異圖）寫到建置目錄，不污染原始碼樹。
        artifactDir_ = QDir::currentPath() + QStringLiteral("/render_regression_artifacts");

        corpusDir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(corpusDir_->isValid());
        corpusPath_ = corpusDir_->filePath(QStringLiteral("golden_corpus.pdf"));

        // 語料每次現產而不是存成檔案進版控：產生器是決定性的（見 engineering_corpus.h），
        // 存一份幾 MB 的 PDF 進 git 只是把同一個真相放兩個地方。
        const std::string bytes = corpus::generate(corpus::goldenCorpusOptions());
        QFile file(corpusPath_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes.data(), static_cast<qint64>(bytes.size())),
                 static_cast<qint64>(bytes.size()));
        file.close();

        engine_ = std::make_unique<engine::PdfiumEngine>();
        engine::OpenResult opened;
        engine_->openDocument(corpusPath_.toStdString(), "",
                              [&opened](engine::OpenResult r) { opened = std::move(r); });
        engine_->waitForIdle();
        QVERIFY2(opened.ok(), "基準語料開檔失敗");
    }

    void cleanupTestCase() {
        if (engine_) {
            engine_->closeDocument();
            engine_->waitForIdle();
        }
        engine_.reset();
    }

    // 產生器是決定性的：同樣的參數要產生同樣的位元組。
    // 這是整個基準影像框架的前提，先驗它，否則基準失敗會被誤讀成渲染退化。
    void corpusGeneratorIsDeterministic() {
        const std::string first = corpus::generate(corpus::goldenCorpusOptions());
        const std::string second = corpus::generate(corpus::goldenCorpusOptions());
        QCOMPARE(first.size(), second.size());
        QVERIFY2(first == second,
                 "語料產生器輸出不一致，基準影像框架整個不成立");
    }

    void renderMatchesGolden_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("pageIndex");
        QTest::addColumn<double>("scale");
        QTest::addColumn<bool>("nightMode");
        QTest::addColumn<bool>("thinLines");

        // 兩頁各一張（語料 variantCount = 2，兩頁內容不同）。
        QTest::newRow("page0-100pct") << QStringLiteral("page0_100pct") << 0 << 1.0 << false << false;
        QTest::newRow("page1-100pct") << QStringLiteral("page1_100pct") << 1 << 1.0 << false << false;
        // 縮小：細線在低倍率下是否消失，是工程圖最常見的抱怨（PRD-VIEW-010）。
        QTest::newRow("page0-50pct") << QStringLiteral("page0_50pct") << 0 << 0.5 << false << false;
        // 夜間模式（PRD-VIEW-007）。
        QTest::newRow("page0-night") << QStringLiteral("page0_night") << 0 << 0.5 << true << false;
        // 細線增強（PRD-VIEW-010）。目前 thinLines 在引擎裡是刻意的 no-op
        // （見 pdfium_engine.cpp：它屬於外觀層處置而非 PDFium 旗標），
        // 因此這張基準目前與 page0_50pct 逐位元組相同。
        // 這個情境仍然留著：等 PRD-VIEW-010 真的實作時，這一項會失敗並要求重新確認基準，
        // 而那正是我們要的——渲染行為改變必須有人看過才算數。
        QTest::newRow("page0-thin") << QStringLiteral("page0_thin") << 0 << 0.5 << false << true;
    }

    void renderMatchesGolden() {
        QFETCH(QString, name);
        QFETCH(int, pageIndex);
        QFETCH(double, scale);
        QFETCH(bool, nightMode);
        QFETCH(bool, thinLines);

        std::optional<domain::PageInfo> info;
        engine_->pageInfo(pageIndex, [&info](std::optional<domain::PageInfo> value) {
            info = std::move(value);
        });
        engine_->waitForIdle();
        QVERIFY(info.has_value());

        engine::RenderOptions options;
        options.nightMode = nightMode;
        options.thinLines = thinLines;

        const QImage actual = renderPageByTiles(*engine_, *info, scale, options);
        QVERIFY2(!actual.isNull(), "渲染沒有產生任何像素");

        const QString goldenPath = goldenDir_ + QLatin1Char('/') + name + QStringLiteral(".png");
        if (!QFileInfo::exists(goldenPath)) {
            QVERIFY2(test::saveImage(goldenPath, actual),
                     qPrintable(QStringLiteral("無法寫入基準影像 ") + goldenPath));
            // 建立基準不算通過。若這裡回報成功，第一次執行的綠燈會讓人以為
            // 渲染已被驗過，而實際上什麼都沒比對。
            QSKIP(qPrintable(
                QStringLiteral("已建立基準，本次未比對：%1\n"
                               "  請人工確認這張影像是正確的畫面後再提交進版控；"
                               "錯的基準會把錯的畫面永遠釘住。")
                    .arg(goldenPath)));
        }

        QImage golden;
        QVERIFY2(golden.load(goldenPath), qPrintable(QStringLiteral("基準影像讀取失敗 ") + goldenPath));
        golden = golden.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        const test::ImageComparison comparison = test::compareImages(actual, golden);
        if (!comparison.acceptable()) {
            // 失敗時一定要留下可看的產物：只給一個 SSIM 數字沒有人能判斷壞在哪裡。
            const QString actualPath =
                artifactDir_ + QLatin1Char('/') + name + QStringLiteral("_actual.png");
            test::saveImage(actualPath, actual);
            QString message =
                QStringLiteral("渲染與基準不符：%1\n  SSIM = %2（門檻 %3）\n"
                               "  最大通道差 = %4，差異像素比例 = %5\n  實際輸出：%6")
                    .arg(name)
                    .arg(comparison.ssim, 0, 'f', 6)
                    .arg(test::kGoldenSsimThreshold, 0, 'f', 4)
                    .arg(comparison.maxChannelDelta)
                    .arg(comparison.differingPixelRatio, 0, 'f', 6)
                    .arg(actualPath);
            if (!comparison.sizeMatches) {
                message += QStringLiteral("\n  尺寸不符：實際 %1×%2，基準 %3×%4")
                               .arg(actual.width())
                               .arg(actual.height())
                               .arg(golden.width())
                               .arg(golden.height());
            } else {
                const QString diffPath =
                    artifactDir_ + QLatin1Char('/') + name + QStringLiteral("_diff.png");
                test::saveImage(diffPath, test::makeDiffImage(actual, golden));
                message += QStringLiteral("\n  差異圖：%1").arg(diffPath);
            }
            QFAIL(qPrintable(message));
        }
    }

    // 比對函式本身要能抓到差異，否則「全部通過」也可能只是因為它永遠回傳通過。
    void comparisonDetectsInjectedDifference() {
        QImage a(64, 64, QImage::Format_ARGB32_Premultiplied);
        a.fill(Qt::white);
        for (int i = 0; i < 64; ++i) a.setPixel(i, i, qRgb(0, 0, 0));

        QImage b = a.copy();
        QVERIFY(test::compareImages(a, b).acceptable());

        // 抹掉一條線 —— 這正是「渲染退化」的典型樣貌。
        for (int i = 0; i < 64; ++i) b.setPixel(i, i, qRgb(255, 255, 255));
        const test::ImageComparison broken = test::compareImages(a, b);
        QVERIFY2(!broken.acceptable(),
                 qPrintable(QStringLiteral("整條線消失卻仍判定通過，SSIM = %1")
                                .arg(broken.ssim, 0, 'f', 6)));
    }

private:
    QString goldenDir_;
    QString artifactDir_;
    QString corpusPath_;
    std::unique_ptr<QTemporaryDir> corpusDir_;
    std::unique_ptr<engine::PdfiumEngine> engine_;
};

QTEST_MAIN(TestRenderRegression)
#include "test_render_regression.moc"
