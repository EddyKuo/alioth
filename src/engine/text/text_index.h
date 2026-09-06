#pragma once

// 預建文字索引（PRD-SRCH-001：500 頁 ≤ 2 秒）。ADR-005 指定的路徑。
//
// 為什麼不是並行搜尋：量測顯示 PDFium 的文件把手**可以並存、不可以並行**，
// 兩條執行緒同時動作會非決定性地掉資料。詳見 ADR-005。
//
// 為什麼預建索引解得掉這個預算：循序搜尋 500 頁要 4 秒，成本不在比對字串，
// 而在 PDFium 每頁都得把含數千條向量的內容串流解析一遍才拿得到文字層。
// 那份解析結果**每次搜尋都重來一次**，而它其實不會變。抽出來存成純文字之後，
// 搜尋只是在記憶體裡比對字串，與 PDFium 完全無關，也就不需要任何並行。
//
// 索引以 char32_t 逐字元存放，位置**直接就是 PDFium 的字元索引**。
// 這個 1:1 對應是刻意的，與 domain/text_layer.h 的理由相同：搜尋結果要能換算成
// 選取範圍與 QuadPoints，中間一旦多一層自訂編號，任何一次不一致都會讓
// 螢光筆標到別的位置。因此這裡不做正規化、不合併空白、不丟控制字元。
//
// 記憶體：每字元 4 位元組。500 頁工程圖（每頁約 320 個文字物件）約數 MB；
// 十萬字的文字型文件約 400 KB。相對於 512 MB 的閒置預算可以忽略。

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/text_layer.h"
#include "engine/cancellation.h"
#include "engine/text/text_extractor.h"
#include "engine/text/text_search.h"

namespace alioth::engine::text {

// 建索引的進度。indexedPages 到達 totalPages 就是建完。
struct IndexProgress {
    std::int32_t indexedPages{0};
    std::int32_t totalPages{0};
    bool cancelled{false};
};

class TextIndex {
public:
    // 索引一頁。text 的第 i 個元素必須對應 PDFium 的第 i 個字元。
    void setPage(std::int32_t pageIndex, std::u32string text);

    [[nodiscard]] bool hasPage(std::int32_t pageIndex) const;
    [[nodiscard]] std::int32_t indexedPageCount() const;
    [[nodiscard]] bool isEmpty() const { return indexedPageCount() == 0; }
    void clear();

    // 從 PageTextLayer 取字元。與 setPage 等價，只是省掉呼叫端自己組字串。
    void setPageFromLayer(const domain::PageTextLayer& layer);

    // 全文搜尋。只在已索引的頁面上比對——**未索引的頁面會被略過，不是回報無命中**，
    // 呼叫端必須靠 indexedPageCount() 判斷結果是否完整，否則會把「還沒建完」
    // 顯示成「找不到」，而那正是搜尋最不能犯的錯。
    //
    // startPage 之後的頁面先掃，然後繞回開頭，與 SearchSession 的順序一致。
    [[nodiscard]] std::vector<domain::SearchResult> search(
        const std::string& queryUtf8, const SearchOptions& options, std::int32_t startPage = 0,
        const CancellationToken& token = {}) const;

private:
    // 索引由文字執行緒逐頁寫入，而搜尋是從 GUI 執行緒問的——兩者必然交錯。
    // 鎖的持有時間是「複製一頁的指標」或「掃一次索引」，而掃一次是毫秒級，
    // 不會讓建索引明顯變慢。沒有這把鎖的話，症狀是搜尋時偶發崩潰或少幾頁，
    // 而且只在大文件上出現——也就是最不容易在測試裡重現的那種。
    mutable std::mutex mutex_;
    std::unordered_map<std::int32_t, std::u32string> pages_;
};

// 逐頁建索引。**單一擷取器、單一執行緒**——這是 ADR-005 的硬性條件，
// 不要為了快而在這裡開第二條執行緒。
//
// 一次排一頁的工作，讓取消能在頁與頁之間生效；一次全排會讓取消要等整份做完。
class TextIndexBuilder {
public:
    using ProgressCallback = std::function<void(IndexProgress)>;

    explicit TextIndexBuilder(TextExtractor& extractor);
    ~TextIndexBuilder();

    TextIndexBuilder(const TextIndexBuilder&) = delete;
    TextIndexBuilder& operator=(const TextIndexBuilder&) = delete;

    // 回呼在文字執行緒上被呼叫，呼叫端須自行排回自己的執行緒。
    // onProgress 每完成一頁呼叫一次，最後一次的 indexedPages == totalPages。
    void start(TextIndex& index, std::int32_t pageCount, ProgressCallback onProgress);

    void cancel();
    [[nodiscard]] bool isRunning() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace alioth::engine::text
