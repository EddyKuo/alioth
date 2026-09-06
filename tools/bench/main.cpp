// Alioth 效能基準工具（WBS 7.4）。
//
// PRD §9 要求每次 PR 與每日夜間執行，退步超過 10% 阻擋合併，因此輸出必須是機器可讀的
// JSON 而不是給人看的文字。這裡量的是 PRD §8.1 表格裡真正定義了數字的那幾項。
//
// 本版相對於初版的三項改動，理由都寫在 status.md 的「已知缺口」裡：
//
//   一、語料換成 tools/corpus 的工程圖產生器。初版每頁只有 24 個矩形，單張圖磚
//       0.4 毫秒，量到的是排程成本而不是渲染成本。用它宣告 §8.1 達標等於用空白頁
//       證明印表機很快。新語料是 A0 幅面、每頁數千條折線、含虛線與剖面線與尺寸標註。
//
//   二、加入記憶體量測。§8.1 有兩條記憶體驗收條件，之前完全沒量。
//       作業系統呼叫收在 src/platform/process_metrics.h，本工具只呼叫那個介面
//       ——CLAUDE.md 的分層規則對工具同樣有效，不因為「這只是個 bench」而放寬。
//
//   三、指標從兩項擴到六項，涵蓋 §8.1 表格裡本行程量得到的全部：冷啟動、
//       翻頁（已快取／未快取）、縮放至首個新圖磚、閒置記憶體、全文搜尋。
//       量不到的（安裝包大小、捲動幀率、主執行緒阻塞）明確列在 JSON 的
//       notMeasured 欄位裡，不留白讓人以為已經量過。
//
// 仍然刻意不依賴外部語料：--generate 就能在乾淨的機器上跑起來。真實的相容性語料
// （PRD §9 的 ≥ 300 份）是驗收層級的事，不是基準線的事。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include "domain/page_layout.h"
#include "domain/tile.h"
#include "engine/pdfium_engine.h"
#include "engine/text/text_extractor.h"
#include "engine/text/text_index.h"
#include "engine/text/text_search.h"
#include "engine/tile_cache.h"
#include "engineering_corpus.h"
#include "platform/process_metrics.h"

namespace {

using Clock = std::chrono::steady_clock;
using alioth::domain::Rotation;
using alioth::domain::exactScaleKey;
using alioth::domain::TileKey;
using namespace alioth::engine;

// 行程啟動的時間原點。放在最前面量，讓「冷啟動」包含 PDFium 全域初始化——
// 使用者感受到的冷啟動是從雙擊圖示開始的，不是從 openDocument 開始的。
const Clock::time_point kProcessStart = Clock::now();

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

// 可視區的假設。1600×1000 邏輯像素大約是 24 吋螢幕上最大化的檢視區，
// 對應 4×2 張 512 圖磚。翻頁的成本必須以「可見範圍」定義而不是整頁——
// 整頁光柵化本來就是被禁止的（CLAUDE.md 硬性限制 2），拿整頁的成本當翻頁成本
// 會量出一個我們根本不會付的數字。
constexpr int kViewportWidthPx = 1600;
constexpr int kViewportHeightPx = 1000;

std::vector<TileKey> visibleTiles(int pageIndex, double scale, bool nightMode = false) {
    const int columns =
        (std::min(kViewportWidthPx, 4096) + alioth::domain::kTileSize - 1) / alioth::domain::kTileSize;
    const int rows = (std::min(kViewportHeightPx, 4096) + alioth::domain::kTileSize - 1) /
                     alioth::domain::kTileSize;
    std::vector<TileKey> keys;
    keys.reserve(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            keys.push_back(TileKey{pageIndex, exactScaleKey(scale), column, row, Rotation::None,
                                   nightMode});
        }
    }
    return keys;
}

struct Results {
    int pageCount{0};
    double corpusBytes{0.0};

    double openMs{0.0};
    double firstTileMs{0.0};
    double coldStartMs{0.0};          // 行程啟動 → 首頁第一張圖磚
    double pageFlipUncachedP95Ms{0.0};
    double pageFlipCachedP95Ms{0.0};
    double zoomFirstTileP95Ms{0.0};
    double idlePrivateMB{0.0};
    double idleWorkingSetMB{0.0};
    double peakWorkingSetMB{0.0};
    bool memoryValid{false};
    double searchMs{0.0};
    double searchMsPer500Pages{0.0};
    int searchSamples{0};
    double searchSpreadMs{0.0};
    int searchMatches{0};
    int searchPagesScanned{0};

    // 預建索引路徑（ADR-005）。indexBuildMs 是開檔後一次性的成本，
    // indexedSearchMs 是使用者每按一次搜尋真正等待的時間——PRD-SRCH-001
    // 的 2 秒預算管的是後者。兩個都要報，只報後者會讓一次性成本消失在帳面上。
    double indexBuildMs{0.0};
    double indexedSearchMs{0.0};
    double indexedSearchMsPer500Pages{0.0};
    int indexedSearchMatches{0};
    int indexedPages{0};

    double tileP50Ms{0.0};
    double tileP95Ms{0.0};
    double tilesPerSecond{0.0};
    int tilesRendered{0};
    double scrollHitRate{0.0};
};

// 一次同步的圖磚渲染。基準工具刻意逐張等待：要量的是單張圖磚的成本分佈，
// 讓佇列排隊會把排程延遲混進渲染時間裡，量出來的 P95 反映的是佇列深度而不是渲染。
double renderOne(PdfiumEngine& engine, TileCache& cache, const TileKey& key) {
    const auto start = Clock::now();
    engine.renderTile(key, RenderOptions{}, alioth::domain::TaskPriority::Visible,
                      CancellationToken{}, [&cache](RenderResult r) {
                          if (r.ok()) cache.insert(r.key, r.buffer);
                      });
    engine.waitForIdle();
    return msSince(start);
}

// 全文搜尋（PRD §8.1：≤ 2 秒 / 500 頁）。
//
// 走 SearchSession 而不是自己逐頁呼叫 searchPage：正式路徑就是它，
// 而逐頁增量與頁間取消的成本正是要量的東西。文字子系統自持獨立文件把手與執行緒，
// 因此這裡的計時不受渲染佇列影響——那也正是它被設計成獨立把手的理由。
// 單次搜尋的耗時。回傳毫秒；開檔失敗回傳負值。
double measureSearchOnce(const std::string& path, Results& results) {
    text::TextExtractor extractor;

    std::mutex mutex;
    std::condition_variable cv;
    bool opened = false;
    alioth::domain::DocumentError openError = alioth::domain::DocumentError::None;

    extractor.open(path, "", [&](alioth::domain::DocumentError error) {
        std::lock_guard<std::mutex> lock(mutex);
        openError = error;
        opened = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return opened; });
    }
    if (openError != alioth::domain::DocumentError::None) return -1.0;

    text::SearchSession session(extractor);
    text::SearchSummary summary;
    bool finished = false;

    // 查一個一定會命中、而且每頁都命中的字串：命中數為零的搜尋會提早在
    // 「找不到任何字」的路徑上結束，量到的是空轉。語料的標題欄一定含 ALIOTH。
    const auto start = Clock::now();
    session.start("ALIOTH", text::SearchOptions{}, 0,
                  [](std::int32_t, std::vector<alioth::domain::SearchResult>) {},
                  [&](text::SearchSummary s) {
                      std::lock_guard<std::mutex> lock(mutex);
                      summary = s;
                      finished = true;
                      cv.notify_all();
                  });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return finished; });
    }
    const double elapsed = msSince(start);
    results.searchMatches = summary.totalMatches;
    results.searchPagesScanned = summary.pagesScanned;

    extractor.close();
    extractor.waitForIdle();
    return elapsed;
}

// 預建索引路徑（ADR-005）。量兩件事，因為它們是兩種不同的等待：
//   indexBuildMs    開檔後一次性的背景成本，使用者不會盯著它等
//   indexedSearchMs 每按一次搜尋真正的等待時間——PRD-SRCH-001 管的是這個
//
// 只報後者會讓一次性成本從帳面上消失，那是在自己騙自己。
void measureIndexedSearch(const std::string& path, Results& results) {
    text::TextExtractor extractor;

    std::mutex mutex;
    std::condition_variable cv;
    bool opened = false;
    alioth::domain::DocumentError openError = alioth::domain::DocumentError::None;

    extractor.open(path, "", [&](alioth::domain::DocumentError error) {
        std::lock_guard<std::mutex> lock(mutex);
        openError = error;
        opened = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return opened; });
    }
    if (openError != alioth::domain::DocumentError::None) return;

    text::TextIndex index;
    text::TextIndexBuilder builder(extractor);
    const auto buildStart = Clock::now();
    builder.start(index, extractor.pageCount(), {});
    // 建索引全在文字執行緒上排隊，等佇列清空就是建完。
    extractor.waitForIdle();
    results.indexBuildMs = msSince(buildStart);
    results.indexedPages = index.indexedPageCount();

    // 搜尋本身量三次取中位數。它應該是毫秒級，而毫秒級的量測最容易被
    // 單次排程尖峰扭曲。
    std::vector<double> samples;
    for (int i = 0; i < 3; ++i) {
        const auto start = Clock::now();
        const auto hits = index.search("ALIOTH", text::SearchOptions{}, 0);
        samples.push_back(msSince(start));
        results.indexedSearchMatches = static_cast<int>(hits.size());
    }
    std::sort(samples.begin(), samples.end());
    results.indexedSearchMs = samples[samples.size() / 2];
    if (results.indexedPages > 0) {
        results.indexedSearchMsPer500Pages =
            results.indexedSearchMs * 500.0 / static_cast<double>(results.indexedPages);
    }

    extractor.close();
    extractor.waitForIdle();
}

// 搜尋取多次的中位數。
//
// 這一項的自然變異實測是 3985–4789 毫秒（約 20%），單次量測搭配 10% 的退步門檻
// 必然誤報——而一個會誤報的閘門，人們學會的是忽略它，真正的退步也就跟著被蓋掉。
// 其餘指標的變異都在 3% 以內，不需要重複量測。
//
// 取中位數而不是平均：偶爾一次的排程尖峰會把平均拉高，中位數不受單一離群值影響。
void measureSearch(const std::string& path, Results& results, int repeats) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(std::max(1, repeats)));
    for (int i = 0; i < std::max(1, repeats); ++i) {
        const double elapsed = measureSearchOnce(path, results);
        if (elapsed >= 0.0) samples.push_back(elapsed);
    }
    if (samples.empty()) return;

    std::sort(samples.begin(), samples.end());
    results.searchMs = samples[samples.size() / 2];
    results.searchSamples = static_cast<int>(samples.size());
    results.searchSpreadMs = samples.back() - samples.front();

    if (results.searchPagesScanned > 0) {
        results.searchMsPer500Pages =
            results.searchMs * 500.0 / static_cast<double>(results.searchPagesScanned);
    }
}

Results run(const std::string& path, int scrollSteps, bool measureSearchMetric,
           int searchRepeats) {
    Results results;
    PdfiumEngine engine;
    TileCache cache;

    // 冷啟動到首頁可見（PRD §8.1 ≤ 2.0 秒 P95）。
    const auto openStart = Clock::now();
    OpenResult openResult;
    bool openDone = false;
    engine.openDocument(path, "", [&](OpenResult r) {
        openResult = std::move(r);
        openDone = true;
    });
    engine.waitForIdle();
    results.openMs = msSince(openStart);

    if (!openDone || !openResult.ok()) {
        std::fprintf(stderr, "開檔失敗\n");
        return results;
    }
    results.pageCount = openResult.info.pageCount;

    // 首頁可見 = 可視區內的圖磚都畫完，不是第一張圖磚畫完。
    // 只量第一張會低估三到八倍，而使用者要等的是整個可視區。
    const auto firstStart = Clock::now();
    for (const TileKey& key : visibleTiles(0, 1.0)) {
        engine.renderTile(key, RenderOptions{}, alioth::domain::TaskPriority::Visible,
                          CancellationToken{}, [&cache](RenderResult r) {
                              if (r.ok()) cache.insert(r.key, r.buffer);
                          });
    }
    engine.waitForIdle();
    results.firstTileMs = msSince(firstStart);
    results.coldStartMs = msSince(kProcessStart);

    // 翻頁（未快取）：跳到沒去過的頁，畫滿可視區。
    std::vector<double> uncachedFlips;
    std::vector<double> tileTimes;
    const int flipCount = std::min(results.pageCount - 1, 12);
    for (int i = 1; i <= flipCount; ++i) {
        const auto flipStart = Clock::now();
        for (const TileKey& key : visibleTiles(i, 1.0)) {
            if (cache.find(key)) continue;
            tileTimes.push_back(renderOne(engine, cache, key));
        }
        uncachedFlips.push_back(msSince(flipStart));
    }

    // 翻頁（已快取）：回到剛剛去過的頁。量到的是快取查詢，不含渲染——
    // 這正是 §8.1 把兩者分成 50 / 250 毫秒兩欄的原因。
    std::vector<double> cachedFlips;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int i = 1; i <= flipCount; ++i) {
            const auto flipStart = Clock::now();
            bool complete = true;
            for (const TileKey& key : visibleTiles(i, 1.0)) {
                if (!cache.find(key)) complete = false;
            }
            const double elapsed = msSince(flipStart);
            if (complete) cachedFlips.push_back(elapsed);
        }
    }

    // 縮放至首個新圖磚（§8.1 ≤ 150 毫秒 P95）。
    // 用 exactScaleKey 而不是 coarseScaleKey：PRD §4.3 明令拉伸結果不得作為最終畫面，
    // 使用者等的是新倍率下的第一張真圖磚。
    std::vector<double> zoomFirstTiles;
    const double zoomLevels[] = {1.25, 1.5, 2.0, 0.75, 3.0, 0.5};
    for (const double zoom : zoomLevels) {
        const TileKey key{0, exactScaleKey(zoom), 0, 0, Rotation::None, false};
        zoomFirstTiles.push_back(renderOne(engine, cache, key));
    }

    // 往返捲動：單向前進永遠不會命中快取，量出來的命中率對「使用者來回比對圖說」
    // 這個真實情境毫無代表性——而那正是快取要服務的情境。
    const auto throughputStart = Clock::now();
    const int span = std::max(1, std::min(results.pageCount, scrollSteps / 2));
    int throughputTiles = 0;
    for (int step = 0; step < scrollSteps; ++step) {
        const int phase = step % (span * 2);
        const int page = phase < span ? phase : span * 2 - 1 - phase;
        for (const TileKey& key : visibleTiles(page, 1.0)) {
            if (cache.find(key)) continue;
            tileTimes.push_back(renderOne(engine, cache, key));
            ++throughputTiles;
        }
    }
    const double throughputMs = msSince(throughputStart);

    // 閒置記憶體（§8.1 ≤ 512 MB）。先讓佇列清空，量的才是「閒置」而不是「工作中」。
    // 不修剪工作集，理由見 process_metrics.h：修剪後量到的是 0.2 MB，
    // 那個數字不對應工作管理員裡的任何東西，而 PRD 的驗收正是對著那個欄位。
    engine.discardPending(alioth::domain::TaskPriority::Background);
    engine.waitForIdle();
    const alioth::platform::ProcessMemory memory = alioth::platform::currentProcessMemory();
    results.memoryValid = memory.valid;
    constexpr double kMega = 1024.0 * 1024.0;
    results.idlePrivateMB = static_cast<double>(memory.privateBytes) / kMega;
    results.idleWorkingSetMB = static_cast<double>(memory.workingSetBytes) / kMega;
    results.peakWorkingSetMB = static_cast<double>(memory.peakWorkingSetBytes) / kMega;

    results.tilesRendered = static_cast<int>(tileTimes.size());
    results.tileP50Ms = percentile(tileTimes, 0.50);
    results.tileP95Ms = percentile(tileTimes, 0.95);
    // 圖磚吞吐率。以實際渲染過的圖磚時間平均計算，而不是「往返捲動的總時間除以張數」
    // ——往返捲動大部分時間都在命中快取，那個分母會把吞吐率算成任意大的數字。
    const double meanTileMs =
        tileTimes.empty()
            ? 0.0
            : std::accumulate(tileTimes.begin(), tileTimes.end(), 0.0) /
                  static_cast<double>(tileTimes.size());
    results.tilesPerSecond = meanTileMs > 0.0 ? 1000.0 / meanTileMs : 0.0;
    (void)throughputMs;
    (void)throughputTiles;
    results.scrollHitRate = cache.stats().hitRate();
    results.pageFlipUncachedP95Ms = percentile(uncachedFlips, 0.95);
    results.pageFlipCachedP95Ms = percentile(cachedFlips, 0.95);
    results.zoomFirstTileP95Ms = percentile(zoomFirstTiles, 0.95);

    engine.closeDocument();
    engine.waitForIdle();

    // 搜尋另外開一份獨立的文件把手（CLAUDE.md 硬性限制 1），排在渲染量測之後，
    // 避免兩者的記憶體互相污染閒置記憶體這項指標。
    if (measureSearchMetric) {
        measureSearch(path, results, searchRepeats);
        measureIndexedSearch(path, results);
    }

    return results;
}

// PRD §8.1 的預算。數字寫在這裡而不是散在判斷式裡，讓「門檻是多少」只有一個真相來源。
struct Budgets {
    double coldStartMs{2000.0};
    double pageFlipUncachedMs{250.0};
    double pageFlipCachedMs{50.0};
    double zoomFirstTileMs{150.0};
    double idlePrivateMB{512.0};
    double searchMsPer500Pages{2000.0};
};

void printJson(const Results& r, const Budgets& b, const std::string& corpusLabel) {
    std::printf(
        "{\n"
        "  \"schema\": \"alioth.bench/2\",\n"
        "  \"corpus\": \"%s\",\n"
        "  \"pageCount\": %d,\n"
        "  \"corpusBytes\": %.0f,\n"
        "  \"metrics\": {\n"
        "    \"coldStartMs\": %.1f,\n"
        "    \"openMs\": %.1f,\n"
        "    \"firstViewportMs\": %.1f,\n"
        "    \"pageFlipUncachedP95Ms\": %.1f,\n"
        "    \"pageFlipCachedP95Ms\": %.3f,\n"
        "    \"zoomFirstTileP95Ms\": %.1f,\n"
        "    \"idlePrivateMB\": %.1f,\n"
        "    \"idleWorkingSetMB\": %.1f,\n"
        "    \"peakWorkingSetMB\": %.1f,\n"
        "    \"memoryMeasured\": %s,\n"
        "    \"searchMs\": %.1f,\n"
        "    \"searchMsPer500Pages\": %.1f,\n"
        "    \"searchMatches\": %d,\n"
        "    \"searchPagesScanned\": %d,\n"
        "    \"indexBuildMs\": %.1f,\n"
        "    \"indexedSearchMs\": %.1f,\n"
        "    \"indexedSearchMsPer500Pages\": %.1f,\n"
        "    \"indexedSearchMatches\": %d,\n"
        "    \"indexedPages\": %d,\n"
        "    \"tileP50Ms\": %.2f,\n"
        "    \"tileP95Ms\": %.2f,\n"
        "    \"tilesPerSecond\": %.1f,\n"
        "    \"tilesRendered\": %d,\n"
        "    \"scrollCacheHitRate\": %.4f\n"
        "  },\n"
        "  \"budgets\": {\n"
        "    \"coldStartMs\": %.0f,\n"
        "    \"pageFlipUncachedP95Ms\": %.0f,\n"
        "    \"pageFlipCachedP95Ms\": %.0f,\n"
        "    \"zoomFirstTileP95Ms\": %.0f,\n"
        "    \"idlePrivateMB\": %.0f,\n"
        "    \"searchMsPer500Pages\": %.0f\n"
        "  },\n"
        // 量不到的指標明列出來。留白會讓讀 JSON 的人以為 §8.1 已全數涵蓋，
        // 而「以為量過」比「知道沒量」危險得多。
        "  \"notMeasured\": [\n"
        "    \"scrollFps: 需要呈現層與合成器，屬於 UI 自動化測試（WBS 7.7）\",\n"
        "    \"mainThreadBlockMs: 需要在 GUI 事件迴圈上量，本工具無事件迴圈\",\n"
        "    \"installerSizeMB: 屬於發佈流程（WP8）\",\n"
        "    \"incrementalSaveMs: 已由 tests/save/test_large_incremental_save.cpp 量測"
        "（100 MB 文件加一個註解），不在本工具重複\",\n"
        "    \"memory10000Pages: 需要 10,000 頁語料，產生成本高，另行排程\"\n"
        "  ]\n"
        "}\n",
        corpusLabel.c_str(), r.pageCount, r.corpusBytes, r.coldStartMs, r.openMs, r.firstTileMs,
        r.pageFlipUncachedP95Ms, r.pageFlipCachedP95Ms, r.zoomFirstTileP95Ms, r.idlePrivateMB,
        r.idleWorkingSetMB, r.peakWorkingSetMB, r.memoryValid ? "true" : "false", r.searchMs, r.searchMsPer500Pages,
        r.searchMatches, r.searchPagesScanned, r.indexBuildMs, r.indexedSearchMs,
        r.indexedSearchMsPer500Pages, r.indexedSearchMatches, r.indexedPages,
        r.tileP50Ms, r.tileP95Ms, r.tilesPerSecond,
        r.tilesRendered, r.scrollHitRate, b.coldStartMs, b.pageFlipUncachedMs, b.pageFlipCachedMs,
        b.zoomFirstTileMs, b.idlePrivateMB, b.searchMsPer500Pages);
}

// 回傳超出預算的項目數。逐項回報而不是只說「未達標」：
// 退步 10% 阻擋合併的機制要能指出是哪一項退步了。
int reportBudgetViolations(const Results& r, const Budgets& b, bool searchMeasured) {
    int violations = 0;
    const auto check = [&](const char* name, double value, double budget, const char* unit) {
        if (value > budget) {
            std::fprintf(stderr, "超出預算: %s = %.1f %s（上限 %.1f）\n", name, value, unit,
                         budget);
            ++violations;
        }
    };

    check("coldStart", r.coldStartMs, b.coldStartMs, "ms");
    check("pageFlipUncachedP95", r.pageFlipUncachedP95Ms, b.pageFlipUncachedMs, "ms");
    check("pageFlipCachedP95", r.pageFlipCachedP95Ms, b.pageFlipCachedMs, "ms");
    check("zoomFirstTileP95", r.zoomFirstTileP95Ms, b.zoomFirstTileMs, "ms");
    if (r.memoryValid) {
        check("idlePrivate", r.idlePrivateMB, b.idlePrivateMB, "MB");
    } else {
        // 量不到記憶體不能當成通過。靜默略過正是 CI 綠燈卻什麼都沒驗的來源。
        std::fprintf(stderr, "記憶體量測失敗，idlePrivate 未驗證\n");
        ++violations;
    }
    if (searchMeasured) {
        check("searchPer500Pages", r.searchMsPer500Pages, b.searchMsPer500Pages, "ms");
    }
    return violations;
}

}  // namespace

// 與基準線比對。回傳是否通過。
//
// 只比會隨程式碼變動的量測值，不比語料大小之類的常數。容忍度 10% 來自 PRD §9；
// 低於 1 毫秒的量測不參與比較——那個尺度的抖動來自排程而不是我們的程式碼。
bool compareWithBaseline(const Results& r, const std::string& path, bool update) {
    struct Metric {
        const char* name;
        double value;
    };
    const Metric metrics[] = {
        {"coldStartMs", r.coldStartMs},
        {"pageFlipUncachedP95Ms", r.pageFlipUncachedP95Ms},
        {"zoomFirstTileP95Ms", r.zoomFirstTileP95Ms},
        {"tileP95Ms", r.tileP95Ms},
        {"idlePrivateMB", r.memoryValid ? r.idlePrivateMB : 0.0},
        {"searchMs", r.searchMs},
        {"indexBuildMs", r.indexBuildMs},
        {"indexedSearchMs", r.indexedSearchMs},
    };

    if (update) {
        std::FILE* out = std::fopen(path.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "無法寫入基準線 %s\n", path.c_str());
            return false;
        }
        std::fprintf(out, "{\n");
        for (std::size_t i = 0; i < std::size(metrics); ++i) {
            std::fprintf(out, "  \"%s\": %.3f%s\n", metrics[i].name, metrics[i].value,
                         i + 1 < std::size(metrics) ? "," : "");
        }
        std::fprintf(out, "}\n");
        std::fclose(out);
        std::fprintf(stderr, "已更新基準線 %s\n", path.c_str());
        return true;
    }

    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "找不到基準線 %s，本次只回報不阻擋\n", path.c_str());
        return true;
    }
    std::string content;
    char buffer[1024];
    while (std::size_t got = std::fread(buffer, 1, sizeof(buffer), in)) {
        content.append(buffer, got);
    }
    std::fclose(in);

    int regressions = 0;
    for (const Metric& metric : metrics) {
        const std::string key = std::string("\"") + metric.name + "\":";
        const std::size_t at = content.find(key);
        if (at == std::string::npos) continue;
        const double previous = std::atof(content.c_str() + at + key.size());
        if (previous < 1.0) continue;  // 太小的量測，抖動來自排程不是程式碼
        const double ratio = metric.value / previous;
        if (ratio > 1.10) {
            std::fprintf(stderr, "退步: %s %.1f -> %.1f（+%.0f%%，上限 10%%）\n", metric.name,
                         previous, metric.value, (ratio - 1.0) * 100.0);
            ++regressions;
        }
    }

    if (regressions > 0) {
        std::fprintf(stderr, "有 %d 項效能退步超過 10%%，阻擋合併\n", regressions);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::string path;
    int generatePages = 0;
    int searchRepeats = 3;
    std::string baselinePath;
    bool updateBaseline = false;
    int scrollSteps = 12;
    bool gate = true;
    bool withSearch = true;
    bool withImage = false;
    bool keepCorpus = false;
    alioth::corpus::CorpusOptions corpusOptions;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--generate") == 0 && i + 1 < argc) {
            generatePages = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            scrollSteps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--polylines") == 0 && i + 1 < argc) {
            corpusOptions.polylines = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--variants") == 0 && i + 1 < argc) {
            corpusOptions.variantCount = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--images") == 0) {
            withImage = true;
        } else if (std::strcmp(argv[i], "--no-search") == 0) {
            withSearch = false;
        } else if (std::strcmp(argv[i], "--search-repeats") == 0 && i + 1 < argc) {
            searchRepeats = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) {
            baselinePath = argv[++i];
        } else if (std::strcmp(argv[i], "--update-baseline") == 0) {
            updateBaseline = true;
        } else if (std::strcmp(argv[i], "--no-gate") == 0) {
            gate = false;
        } else if (std::strcmp(argv[i], "--keep-corpus") == 0) {
            keepCorpus = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "用法: alioth_bench [選項] [檔案.pdf]\n"
                "  --generate <頁數>    產生 A0 工程圖合成語料並以它量測\n"
                "  --polylines <數量>   每頁折線數（預設 2400）\n"
                "  --variants <數量>    不同內容串流的數量（預設 8）\n"
                "  --images             語料嵌入掃描影像\n"
                "  --steps <步數>       往返捲動步數（預設 12）\n"
                "  --no-search          略過全文搜尋量測\n"
                "  --no-gate            只輸出數字，超出預算也以 0 結束\n"
                "  --keep-corpus        量測後保留產生的語料檔\n"
                "輸出 JSON 至 stdout，供 CI 比對基準線；超出預算以非零碼結束。\n");
            return 0;
        } else {
            path = argv[i];
        }
    }

    std::string corpusLabel = "external";
    std::string generatedPath;
    double corpusBytes = 0.0;

    if (generatePages > 0) {
        corpusOptions.pageCount = generatePages;
        corpusOptions.embedImage = withImage;
        generatedPath = "alioth_bench_a0_" + std::to_string(generatePages) + "p.pdf";
        const std::string pdf = alioth::corpus::generate(corpusOptions);
        std::ofstream out(generatedPath, std::ios::binary);
        out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
        out.close();
        path = generatedPath;
        corpusBytes = static_cast<double>(pdf.size());
        corpusLabel = "synthetic-a0-engineering";
    }

    if (path.empty()) {
        std::fprintf(stderr, "需要一個 PDF 路徑，或用 --generate 產生合成語料\n");
        return 2;
    }

    Results results = run(path, scrollSteps, withSearch, searchRepeats);
    results.corpusBytes = corpusBytes;

    const Budgets budgets;
    printJson(results, budgets, corpusLabel);

    if (!generatedPath.empty() && !keepCorpus) {
        std::remove(generatedPath.c_str());
    }

    if (results.pageCount == 0) return 2;

    // 預算違反一律逐項回報，讓它在 CI 輸出裡看得見。
    const int violations = reportBudgetViolations(results, budgets, withSearch);

    if (!gate) return 0;

    // 閘門判定用的是 PRD §9 定義的規則：「退步超過 10% 阻擋合併」，
    // 而不是「超出絕對預算就擋」。差別很實際——目前全文搜尋確實超出 §8.1 的預算
    // （已立案 EXC_20260905_RD_SA_parallel_search），若用絕對預算當閘門，
    // CI 會在修好之前天天紅，然後所有人開始無視它，真正的退步也就跟著被蓋掉。
    //
    // 沒有基準線時只回報不阻擋：第一次跑不該擋下任何人。
    if (baselinePath.empty()) {
        if (violations > 0) {
            std::fprintf(stderr,
                         "有 %d 項超出 PRD 8.1 的預算（見上）。未提供基準線，本次不阻擋。\n"
                         "要啟用退步閘門：--baseline <檔案>，並以 --update-baseline 建立。\n",
                         violations);
        }
        return 0;
    }

    return compareWithBaseline(results, baselinePath, updateBaseline) ? 0 : 1;
}
