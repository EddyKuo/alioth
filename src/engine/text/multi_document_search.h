#pragma once

// 多文件搜尋（PRD-SRCH-002）——逐份序列處理，刻意不並行。
//
// **這不是「還沒優化」，是排查 exceptions/EXC_20260905_RD_SA_parallel_search.md
// 之後得到的結論**，理由分兩層：
//
// 一、已觀察到的症狀。`parallel_search.h`（4 個工作者、各自獨立文件把手與執行緒，
//    完全遵照 SDD §1.1 的做法）在 4 個工作者同時開**同一份檔案**時會漏頁、部分
//    已掃頁面回傳空結果，且後續在同一行程裡新開的文件把手開始無聲失敗，測試中
//    觀察到的失敗形態包含堆積毀損（0xc0000374）與 SEGFAULT——這些是資料競賽
//    （data race）的典型指紋，不是邏輯錯誤的指紋。
//
// 二、更根本的原因（本次排查新找到的證據）。第三方套件本身的公開標頭
//    third_party/pdfium/include/fpdfview.h 開頭明白寫著：
//
//        "NOTE: None of the PDFium APIs are thread-safe. They expect to be
//        called from a single thread. Barring that, embedders are required
//        to ensure (via a mutex or similar) that only a single PDFium call
//        can be made at a time."
//
//    注意這句話沒有把「不同文件」排除在外——它說的是整個行程只能有一個 PDFium
//    呼叫同時執行，不論呼叫的是不是同一份 FPDF_DOCUMENT。SDD §1.1「每個子系統
//    各自持有獨立把手與執行緒」這個做法之所以在目前的四個子系統（渲染、文字、
//    註解、儲存）大致上相安無事，極可能只是因為它們的呼叫時間點很少真的重疊
//    （使用者一次只做一件事），不是因為 PDFium 真的保證了跨執行緒安全。
//    exceptions 文件裡記的「兩份不同檔案、兩條執行緒，連續三次成功」不是這個
//    做法安全的證據，只是併發窗口恰好沒對齊——競賽條件的典型行為就是「大部分
//    時候不出事」。
//
//    這個發現直接回答了那份 exception 提出的問題（「要繼續投資在並行搜尋，還是
//    改走別的路徑？」）：**沒有安全的路徑能讓多份文件在多條執行緒上真正同時呼叫
//    PDFium**，除非在所有呼叫點外面套一個行程級的互斥鎖——而那樣做之後，所有
//    「並行」都會在鎖上排隊，等於失去並行的意義，只留下並行程式碼的複雜度與
//    風險。因此多文件搜尋在架構上就應該是逐份序列，這不是效能上的讓步，是
//    PDFium 的執行緒安全承諾（或者說，缺乏承諾）決定的結果。
//
// 實作上刻意重用 TextExtractor 既有的序列化機制，而不是自己另開一條執行緒：
// TextExtractor 的工作佇列本來就保證同一時間只有一個工作在它的專用執行緒上執行，
// 逐份開檔、逐頁搜尋、開下一份before都會自然排隊，不需要額外的同步。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/document.h"
#include "domain/text_layer.h"
#include "engine/text/text_search.h"

namespace alioth::engine::text {

struct MultiDocumentQuery {
    std::vector<std::string> paths;
    std::string password;  // 目前所有文件共用同一組密碼；逐份設密碼留給未來需求
    std::string queryUtf8;
    SearchOptions options;
};

struct MultiDocumentStats {
    std::int32_t documentsSearched{0};
    std::int32_t documentsSkipped{0};  // 開檔失敗（密碼錯誤、非 PDF 等），跳過不中止整次搜尋
    std::int32_t pagesScanned{0};
    std::int32_t totalMatches{0};
    bool cancelled{false};
};

class MultiDocumentSearch {
public:
    // documentIndex 對應 MultiDocumentQuery::paths 的索引。
    using DocumentCallback = std::function<void(std::size_t documentIndex, domain::DocumentError)>;
    using PageCallback = std::function<void(std::size_t documentIndex, std::int32_t pageIndex,
                                             std::vector<domain::SearchResult>)>;
    using FinishedCallback = std::function<void(MultiDocumentStats)>;

    MultiDocumentSearch();
    ~MultiDocumentSearch();

    MultiDocumentSearch(const MultiDocumentSearch&) = delete;
    MultiDocumentSearch& operator=(const MultiDocumentSearch&) = delete;

    // 所有回呼都在同一條文字執行緒上被呼叫（與 SearchSession 同一個慣例），
    // 呼叫端須自行排回自己的執行緒才能碰 UI 狀態。
    void start(MultiDocumentQuery query, DocumentCallback onDocumentOpened, PageCallback onPage,
              FinishedCallback onFinished);

    // 取消在「文件與文件之間」及「頁與頁之間」生效，最壞情況是多開一份文件或
    // 多掃一頁——與 SearchSession 的取消粒度是同一個道理。
    void cancel();
    [[nodiscard]] bool isRunning() const noexcept;

    // 同步等待，僅供測試使用。
    void waitForIdle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::text
