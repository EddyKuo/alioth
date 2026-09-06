#pragma once

// 增量儲存器（PRD-IO-001、WBS 5.1）。
//
// 為什麼預設走增量而不是重寫整份：數位簽章覆蓋的是簽章當下那一段位元組範圍
// （/ByteRange）。只要原檔的位元組一個都沒被改動、變更只以新物件追加在尾端，
// 既有簽章在 Acrobat 就會顯示為「有效，簽章後有變更」；一旦整份重寫，
// 物件編號與偏移量全部重算，簽章立刻變成「無效」。
// 這是「不破壞既有數位簽章」這個賣點唯一的技術基礎，沒有替代做法。
//
// PDFium 的 FPDF_SaveWithVersion(FPDF_INCREMENTAL) 交給 FPDF_FILEWRITE 回呼的是
// **整份輸出檔**，不是單獨的增量段：它先把原檔位元組原封不動地串過來，再把新物件與
// 新的 xref 接在後面（pdfium 154.0.8035 實測；輸出大小恰為原檔大小加上增量大小）。
// 這個行為決定了兩件事：
//   一、呼叫端不需要、也不可以再自行複製原檔一次，否則會得到兩份前綴。
//   二、就地存檔實際上仍會重寫整份檔案到暫存檔，300 毫秒預算主要花在這裡，
//      與 I/O 頻寬直接相關；「增量 ≤ 20 KB」指的是尾端追加的位元組數。
// 落盤流程因此是：串流寫入暫存 → 落盤同步 → 原子更名（PRD §8.4）。
//
// 因為簽章保全完全建立在「PDFium 真的把原檔前綴原樣串出來」這個行為上，而 PDFium
// 每季升版，saveIncremental 會在串流過程中比對前綴；一旦上游行為改變，
// 結果會回報 fullRewriteFallback 而不是安靜地毀掉使用者的簽章。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/pdfium_library.h"

namespace alioth::engine::save {

// FPDF_DOCUMENT 的不透明別名。本標頭刻意不引入 PDFium 標頭：
// 呼叫端只需要能傳遞把手，不需要（也不該）看見 PDFium 的型別。
using DocumentHandle = void*;

enum class SaveStatus {
    Ok,
    InvalidDocument,
    SourceUnreadable,
    TargetNotWritable,        // 唯讀檔或唯讀目錄（PRD-IO-005：應改走另存）
    TemporaryFileFailed,
    PdfiumWriteFailed,
    CommitFailed,
    RefusedOverwriteOriginal, // 自動儲存絕不覆蓋使用者原檔
};

[[nodiscard]] const char* describe(SaveStatus status) noexcept;

struct SaveOptions {
    // 0 代表沿用原文件版本。PRD §8.3：寫出保留原版本號，不得降級；
    // 因此低於原版的要求值會被無視而不是照做。
    int fileVersion{0};
    bool removeSecurity{false};
};

// PRD-IO-001 的驗收數字（≤ 300 毫秒、增量 ≤ 20 KB）要能被量測到，
// 超標時也要能指出是哪一段慢。
struct SaveMetrics {
    double pdfiumMs{0.0};   // 含 PDFium 串流寫入暫存檔的時間
    double syncMs{0.0};
    double renameMs{0.0};
    double totalMs{0.0};
    std::uint64_t incrementalBytes{0};  // 追加在原檔尾端的位元組數（PRD-IO-001 的 20 KB）
    std::uint64_t sourceBytes{0};
    std::uint64_t totalBytes{0};
};

struct SaveResult {
    SaveStatus status{SaveStatus::Ok};
    SaveMetrics metrics{};
    int fileVersion{0};
    // PDFium 在無法沿用原 xref（例如要求移除加密、xref 曾被重建）時會自行退回整份重寫。
    // 那等於放棄簽章保全，呼叫端必須看得見這件事而不是默默接受。
    bool fullRewriteFallback{false};
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return status == SaveStatus::Ok; }
};

class IncrementalSaver {
public:
    // 增量儲存。targetPath 可以等於 sourcePath（就地存檔），也可以不同
    // （另存一份但保留原有位元組與簽章）。
    static SaveResult saveIncremental(DocumentHandle document, const std::string& sourcePath,
                                      const std::string& targetPath,
                                      const SaveOptions& options = {});

    // 另存新檔：整份重寫（PRD-IO-002 的基礎路徑，也是增量儲存的對照組）。
    // 會重排物件編號，既有簽章因此失效——只在使用者明確選擇另存時走這裡。
    static SaveResult saveAsCopy(DocumentHandle document, const std::string& targetPath,
                                 const SaveOptions& options = {});
};

// 獨立的文件把手。
//
// PDFium 非執行緒安全，且 PdfiumEngine 不對外公開它自己的把手；儲存路徑若要在
// 引擎執行緒之外運作，正確做法是另開一份同檔案的獨立文件（見 CLAUDE.md 硬性限制 1）。
//
// 檔案內容整份讀進記憶體再交給 PDFium，而不是讓 PDFium 自己開檔，理由是就地存檔：
// Windows 上一個被本行程開著的檔案無法被原子更名覆蓋（ReplaceFileW 與 MoveFileExW
// 都會被共用模式擋下），FPDF_LoadDocument 卻會持有該檔案直到文件關閉——
// 兩者相加會讓「存檔到原路徑」永遠失敗。不持有作業系統層的鎖同時也是 PRD-IO-005
// 對雲端同步資料夾相容性的前提。代價是常駐記憶體與檔案大小同階，
// 大型文件之後應改走 FPDF_LoadCustomDocument 搭配可讓出的分段讀取器。
class ScopedDocument {
public:
    ScopedDocument() = default;
    ~ScopedDocument();

    ScopedDocument(const ScopedDocument&) = delete;
    ScopedDocument& operator=(const ScopedDocument&) = delete;
    ScopedDocument(ScopedDocument&& other) noexcept;
    ScopedDocument& operator=(ScopedDocument&& other) noexcept;

    bool open(const std::string& path, const std::string& password = {});
    void close();

    [[nodiscard]] DocumentHandle handle() const noexcept { return document_; }
    [[nodiscard]] explicit operator bool() const noexcept { return document_ != nullptr; }
    [[nodiscard]] int pageCount() const;
    // 17 代表 PDF 1.7。
    [[nodiscard]] int fileVersion() const;
    [[nodiscard]] int signatureCount() const;

private:
    // 引用計數的全域初始化守衛。文件把手活著的期間 PDFium 就不會被別的子系統銷毀。
    std::unique_ptr<PdfiumRuntime> runtime_;
    DocumentHandle document_{nullptr};
    // PDFium 不複製這塊緩衝區，文件存活期間必須保持有效。
    std::vector<unsigned char> bytes_;
};

}  // namespace alioth::engine::save
