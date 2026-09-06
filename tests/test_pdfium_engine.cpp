// 引擎轉接層測試：開檔、零複製圖磚渲染、優先權佇列、取消。
//
// 這些是 M0 的三個 PoC 中前兩個的自動化版本（WBS 1.5 / 1.6）。

#include <QtTest>

#include <QElapsedTimer>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/pdfium_engine.h"
#include "pdf_fixture.h"

using namespace alioth::domain;
using namespace alioth::engine;

namespace {

// 等待非同步結果的小工具。逾時就失敗，不無限期掛住 CI。
template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait(int milliseconds = 10000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }

    [[nodiscard]] const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

bool isWhite(const std::uint8_t* px) {
    return px[0] == 0xFF && px[1] == 0xFF && px[2] == 0xFF;
}

}  // namespace

class TestPdfiumEngine : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        file_ = alioth::test::writeTempPdf(alioth::test::makeSinglePagePdf());
        QVERIFY2(file_ != nullptr, "無法建立測試用 PDF");
        path_ = file_->fileName().toStdString();
    }

    void opensValidDocument() {
        PdfiumEngine engine;
        Latch<OpenResult> latch;
        engine.openDocument(path_, "", [&latch](OpenResult r) { latch.set(std::move(r)); });
        QVERIFY2(latch.wait(), "開檔逾時");
        QVERIFY(latch.value().ok());
        QCOMPARE(latch.value().info.pageCount, 1);
    }

    void reportsMissingFile() {
        PdfiumEngine engine;
        Latch<OpenResult> latch;
        engine.openDocument("no-such-file-hopefully.pdf", "",
                            [&latch](OpenResult r) { latch.set(std::move(r)); });
        QVERIFY(latch.wait());
        QVERIFY(!latch.value().ok());
        QCOMPARE(latch.value().error, DocumentError::FileNotFound);
    }

    void readsPageGeometry() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        Latch<std::optional<PageInfo>> latch;
        engine.pageInfo(0, [&latch](std::optional<PageInfo> info) { latch.set(std::move(info)); });
        QVERIFY(latch.wait());
        QVERIFY(latch.value().has_value());
        QCOMPARE(latch.value()->sizePt.width, 200.0);
        QCOMPARE(latch.value()->sizePt.height, 400.0);
    }

    void rendersTileWithZeroCopyStride() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        Latch<RenderResult> latch;
        engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, false}, RenderOptions{},
                          TaskPriority::Visible, CancellationToken{},
                          [&latch](RenderResult r) { latch.set(std::move(r)); });
        QVERIFY2(latch.wait(), "渲染逾時");

        const RenderResult& result = latch.value();
        QVERIFY(result.ok());
        QCOMPARE(result.buffer->width(), kTileSize);
        QCOMPARE(result.buffer->height(), kTileSize);
        // stride 必須顯式且至少等於寬 × 4——低於此值代表零複製寫入會越界。
        QVERIFY(result.buffer->stride() >= static_cast<std::size_t>(kTileSize) * 4);

        // 頁面高 400 點、圖磚原點在頁面左上：黑色矩形位於頁面下半部（裝置 y 200–400），
        // 左半部（x 0–100）。取 (50, 300) 應該是黑的，(150, 50) 應該是白的。
        const std::uint8_t* black =
            result.buffer->data() + result.buffer->stride() * 300 + 50 * 4;
        const std::uint8_t* white =
            result.buffer->data() + result.buffer->stride() * 50 + 150 * 4;
        QVERIFY2(!isWhite(black), "應為黑色矩形的位置卻是白的——矩陣或 Y 軸方向錯了");
        QVERIFY2(isWhite(white), "應為空白的位置卻不是白的");
    }

    void nightModeInvertsPixels() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        RenderOptions options;
        options.nightMode = true;
        Latch<RenderResult> latch;
        engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, true}, options,
                          TaskPriority::Visible, CancellationToken{},
                          [&latch](RenderResult r) { latch.set(std::move(r)); });
        QVERIFY(latch.wait());
        QVERIFY(latch.value().ok());

        // 原本的白底在夜間模式下應變成黑底。
        const std::uint8_t* px =
            latch.value().buffer->data() + latch.value().buffer->stride() * 50 + 150 * 4;
        QVERIFY(!isWhite(px));
    }

    // PRD-VIEW-007 自訂背景與文字色。
    //
    // 真正要驗的是**接近灰階的像素才會被換色**。整幅無條件替換的實作在
    // 語料上看起來也「有效果」，但那會把照片與圖表一起染成單色，而使用者
    // 換配色是因為白底刺眼，不是要把圖變成單色——那個缺陷只有在真實文件上
    // 才看得出來，語料裡剛好全是黑白就永遠不會被發現。
    void customColorsRemapGreyscaleAndKeepColourAlone() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        RenderOptions options;
        options.customColors = true;
        options.backgroundR = 0xF5;
        options.backgroundG = 0xF0;
        options.backgroundB = 0xE1;
        options.textR = 0x2B;
        options.textG = 0x2B;
        options.textB = 0x2B;

        Latch<RenderResult> latch;
        engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, false}, options,
                          TaskPriority::Visible, CancellationToken{},
                          [&latch](RenderResult r) { latch.set(std::move(r)); });
        QVERIFY2(latch.wait(), "渲染逾時");
        QVERIFY(latch.value().ok());

        const PixelBuffer& buffer = *latch.value().buffer;

        // 原本的白底要變成指定的背景色。
        const std::uint8_t* blank = buffer.data() + buffer.stride() * 50 + 150 * 4;
        QCOMPARE(blank[2], static_cast<std::uint8_t>(0xF5));
        QCOMPARE(blank[1], static_cast<std::uint8_t>(0xF0));
        QCOMPARE(blank[0], static_cast<std::uint8_t>(0xE1));

        // 原本的黑色矩形要變成指定的文字色。
        const std::uint8_t* painted = buffer.data() + buffer.stride() * 300 + 50 * 4;
        QCOMPARE(painted[2], static_cast<std::uint8_t>(0x2B));
        QCOMPARE(painted[1], static_cast<std::uint8_t>(0x2B));
        QCOMPARE(painted[0], static_cast<std::uint8_t>(0x2B));
    }

    // 夜間模式與自訂配色互斥，夜間模式優先。同時套用等於先反相再重新上色，
    // 得到的顏色與使用者選的兩個色都沒有關係。
    void nightModeWinsOverCustomColours() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        RenderOptions options;
        options.nightMode = true;
        options.customColors = true;
        options.backgroundR = options.backgroundG = options.backgroundB = 0xF5;

        Latch<RenderResult> latch;
        engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, true}, options,
                          TaskPriority::Visible, CancellationToken{},
                          [&latch](RenderResult r) { latch.set(std::move(r)); });
        QVERIFY(latch.wait());
        QVERIFY(latch.value().ok());

        // 白底在夜間模式下是全黑，不是 0xF5。
        const std::uint8_t* px =
            latch.value().buffer->data() + latch.value().buffer->stride() * 50 + 150 * 4;
        QCOMPARE(px[2], static_cast<std::uint8_t>(0));
    }

    // PRD-VIEW-018 透明度格線。真正要驗的是**引擎沒有把白底烘進圖磚**——
    // 底色烘死了的話，呈現層畫得再漂亮的棋盤格也永遠透不出來，而那個缺陷
    // 從程式碼上看完全正常。
    void transparencyGridLeavesUnpaintedAreasTransparent() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        RenderOptions options;
        options.transparencyGrid = true;
        Latch<RenderResult> latch;
        engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, false}, options,
                          TaskPriority::Visible, CancellationToken{},
                          [&latch](RenderResult r) { latch.set(std::move(r)); });
        QVERIFY2(latch.wait(), "渲染逾時");
        QVERIFY(latch.value().ok());

        const PixelBuffer& buffer = *latch.value().buffer;
        // (150, 50) 是頁面上沒有任何內容的位置：alpha 必須是 0。
        const std::uint8_t* blank = buffer.data() + buffer.stride() * 50 + 150 * 4;
        QCOMPARE(blank[3], static_cast<std::uint8_t>(0));

        // 有內容的地方必須是不透明的，否則棋盤格會透過黑色矩形本身，
        // 使用者看到的是一塊半透明的髒污。
        const std::uint8_t* painted = buffer.data() + buffer.stride() * 300 + 50 * 4;
        QCOMPARE(painted[3], static_cast<std::uint8_t>(255));
    }

    // 沒開透明度格線時，未繪製的區域仍然要是不透明的白——這是預設行為，
    // 上面那條測試存在的前提。
    void withoutTransparencyGridBackgroundStaysOpaque() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        Latch<RenderResult> latch;
        engine.renderTile(TileKey{0, exactScaleKey(1.0), 0, 0, Rotation::None, false},
                          RenderOptions{}, TaskPriority::Visible, CancellationToken{},
                          [&latch](RenderResult r) { latch.set(std::move(r)); });
        QVERIFY(latch.wait());
        QVERIFY(latch.value().ok());

        const PixelBuffer& buffer = *latch.value().buffer;
        const std::uint8_t* blank = buffer.data() + buffer.stride() * 50 + 150 * 4;
        QCOMPARE(blank[3], static_cast<std::uint8_t>(255));
    }

    void cancelledTilesReportCancellationAndDoNotRender() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        CancellationSource source;
        std::atomic<int> cancelled{0};
        std::atomic<int> rendered{0};

        // 排一批預取任務後立刻取消，模擬使用者連續捲動。
        for (int i = 0; i < 32; ++i) {
            engine.renderTile(TileKey{0, exactScaleKey(1.0), i, 0, Rotation::None, false}, RenderOptions{},
                              TaskPriority::Prefetch, source.token(),
                              [&cancelled, &rendered](RenderResult r) {
                                  if (r.cancelled) {
                                      ++cancelled;
                                  } else if (r.buffer) {
                                      ++rendered;
                                  }
                              });
        }
        source.cancelAll();
        engine.waitForIdle();

        QVERIFY2(cancelled.load() > 0, "取消後應有任務回報被取消");
        QCOMPARE(cancelled.load() + rendered.load(), 32);
    }

    // 取消的延遲上限（PRD-VIEW-003 / CLAUDE.md「任務取消延遲 ≤ 16 毫秒」）。
    //
    // 單塊圖磚渲染中途不可中斷——PDFium 沒有那個 API。這個設計之所以站得住腳，
    // 是因為取消最多只需要等**一塊**已經開跑的圖磚，而不是整批排隊的工作。
    // 這裡驗的就是那件事：取消之後排隊中的工作要被丟掉，不是照樣跑完。
    //
    // 不直接斷言毫秒數：debug 與 release、機器與機器之間差好幾倍，寫死的門檻
    // 不是太鬆就是會偶發紅。改成比較「取消 vs 不取消」的耗時比例——那個比例
    // 由行為決定，不由機器速度決定。
    void cancellingAbandonsQueuedWorkInsteadOfDrainingIt() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        constexpr int kTiles = 48;
        const auto enqueue = [&engine](const CancellationToken& token, std::atomic<int>& rendered) {
            for (int i = 0; i < kTiles; ++i) {
                engine.renderTile(
                    TileKey{0, exactScaleKey(2.0), i, 0, Rotation::None, false}, RenderOptions{},
                    TaskPriority::Prefetch, token, [&rendered](RenderResult r) {
                        if (!r.cancelled && r.buffer) ++rendered;
                    });
            }
        };

        // 基準：全部跑完要多久。
        QElapsedTimer full;
        std::atomic<int> renderedAll{0};
        full.start();
        enqueue(CancellationToken{}, renderedAll);
        engine.waitForIdle();
        const qint64 fullMs = full.elapsed();
        QCOMPARE(renderedAll.load(), kTiles);

        // 取消：排完立刻取消，量到最後一筆回報為止。
        CancellationSource source;
        std::atomic<int> renderedCancelled{0};
        QElapsedTimer cancelled;
        cancelled.start();
        enqueue(source.token(), renderedCancelled);
        source.cancelAll();
        engine.waitForIdle();
        const qint64 cancelledMs = cancelled.elapsed();

        // 取消之後最多只該還在跑「已經開跑的那一塊」。允許少數幾塊是因為
        // 排隊與取消之間本來就有競態——但絕不該接近全部。
        QVERIFY2(renderedCancelled.load() <= kTiles / 4,
                 qPrintable(QStringLiteral("取消後仍渲染了 %1 / %2 塊——排隊中的工作沒有被丟掉")
                                .arg(renderedCancelled.load())
                                .arg(kTiles)));
        // 耗時也要明顯短於全部跑完。只驗數量不驗時間的話，一個「照樣跑完但
        // 回報成取消」的實作會通過，而使用者感受到的仍然是卡住。
        QVERIFY2(cancelledMs * 2 < fullMs || fullMs < 20,
                 qPrintable(QStringLiteral("取消耗時 %1 ms，全部跑完 %2 ms——取消沒有省下時間")
                                .arg(cancelledMs)
                                .arg(fullMs)));
    }

    void discardPendingDropsLowPriorityWork() {
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        std::atomic<int> done{0};
        for (int i = 0; i < 64; ++i) {
            engine.renderTile(TileKey{0, exactScaleKey(1.0), i, 1, Rotation::None, false}, RenderOptions{},
                              TaskPriority::Thumbnail, CancellationToken{},
                              [&done](RenderResult) { ++done; });
        }
        engine.discardPending(TaskPriority::Prefetch);
        engine.waitForIdle();
        QCOMPARE(done.load(), 64);  // 被丟棄的也要回報，不得靜默吞噬（IL-4）
    }

    void concurrentCallersAreSerialisedOnOneThread() {
        // PDFium 非執行緒安全。多個呼叫端同時打進來時，引擎必須把它們
        // 序列化到同一條執行緒上——這裡驗證不會崩潰且結果數量正確。
        PdfiumEngine engine;
        Latch<OpenResult> open;
        engine.openDocument(path_, "", [&open](OpenResult r) { open.set(std::move(r)); });
        QVERIFY(open.wait());

        std::atomic<int> completed{0};
        std::vector<std::thread> callers;
        callers.reserve(8);
        for (int t = 0; t < 8; ++t) {
            callers.emplace_back([&engine, &completed, t] {
                for (int i = 0; i < 8; ++i) {
                    engine.renderTile(TileKey{0, exactScaleKey(1.0), i, t, Rotation::None, false}, RenderOptions{},
                                      TaskPriority::Visible, CancellationToken{},
                                      [&completed](RenderResult) { ++completed; });
                }
            });
        }
        for (auto& thread : callers) thread.join();
        engine.waitForIdle();
        QCOMPARE(completed.load(), 64);
    }

private:
    std::unique_ptr<QTemporaryFile> file_;
    std::string path_;
};

QTEST_APPLESS_MAIN(TestPdfiumEngine)
#include "test_pdfium_engine.moc"
