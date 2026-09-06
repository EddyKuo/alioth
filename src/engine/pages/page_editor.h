#pragma once

// 頁面管理子系統（WBS 5.6–5.8，PRD-PAGE-001/002/003/005/006/012）。
//
// 分工：所有「該做什麼」的判斷都在 domain/page_operations.h（純邏輯、可單獨測），
// 本檔只負責把已經驗證過的意圖翻成 PDFium 呼叫。這樣切的實際好處是頁序與範圍解析
// 的錯誤能在沒有 PDF 的情況下被測出來，而這裡剩下的風險就只有 PDFium 本身的行為。
//
// 執行緒限制與 PdfiumEngine 相同：PDFium 非執行緒安全，同一個 PageEditor 實體
// 只能由單一執行緒使用；要並行處理多份文件就開多個實體，各自持有獨立的文件把手。
//
// 存檔一律轉給 Alioth::save 的 IncrementalSaver，不在這裡自己寫檔：
// 「寫暫存 → 落盤同步 → 原子更名」與簽章保全的前綴檢查只該有一份實作。
//
// 結構性操作（刪頁、重排、插入）能不能增量儲存，取決於 PDFium 願不願意沿用原 xref。
// pdfium 154.0.8035 實測：刪頁與重排之後仍然是增量輸出（原檔位元組完整保留在前面，
// 被刪掉的頁面物件成為檔案中的孤兒），因此既有簽章仍會是「有效，簽章後有變更」。
// 這是實測而非承諾——上游行為可能改變，所以 save() 一律把 fullRewriteFallback
// 原樣交出來，並額外標出本次是否含結構性變更，讓呼叫端能分辨兩種情況。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "domain/page_operations.h"
#include "engine/save/incremental_saver.h"

namespace alioth::engine::pages {

enum class PageEditStatus {
    Ok,
    NotOpen,
    InvalidArgument,   // 領域層驗證未過，細節在 PageEditResult::validation
    PageLoadFailed,    // FPDF_LoadPage 失敗，多半是頁面字典損毀
    PdfiumRejected,    // PDFium API 回傳失敗
    SourceUnreadable,
    SaveFailed,
    Unsupported,       // 已知做不到，且刻意不假裝做得到
};

[[nodiscard]] const char* describe(PageEditStatus status) noexcept;

struct PageEditResult {
    PageEditStatus status{PageEditStatus::Ok};
    domain::pages::OperationStatus validation{domain::pages::OperationStatus::Ok};
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return status == PageEditStatus::Ok; }
};

// 存檔結果。除了 save 子系統原本的資訊，額外標出「本次存檔含結構性變更」——
// 結構性變更 + fullRewriteFallback 是簽章失效的組合，UI 必須能分辨這一種情況
// 與「單純加註解卻意外整份重寫」（後者是 bug，前者是 PDF 格式的必然）。
struct PageSaveResult {
    save::SaveResult save{};
    bool structural{false};

    [[nodiscard]] bool ok() const noexcept { return save.ok(); }
    [[nodiscard]] bool signaturesPreserved() const noexcept {
        return save.ok() && !save.fullRewriteFallback;
    }
};

// 合併／擷取時的表單欄位狀況（WBS 5.7「含表單欄位衝突處理」）。
//
// PDFium 的 FPDF_ImportPagesByIndex 只搬頁面樹與頁面上的註解，**不合併文件層的
// /AcroForm 字典**：Widget 註解會被複製過去，但欄位不再掛在任何 /AcroForm /Fields
// 底下，因此在檢視器裡多半不可填寫。同名欄位的衝突也就無從談起——真正的問題是
// 欄位整個失去登記。這裡如實回報偵測到的 Widget 數量，讓上層能出示警告；
// 真正的 AcroForm 合併需要直接操作物件層，PDFium 公開 API 做不到（見 README 的缺口）。
struct FormFieldReport {
    int widgetAnnotations{0};
    bool acroFormMerged{false};  // 目前恆為 false，見上方說明

    [[nodiscard]] bool hasConflictRisk() const noexcept { return widgetAnnotations > 0; }
};

struct AssemblyResult {
    PageEditStatus status{PageEditStatus::Ok};
    std::string message;
    int pageCount{0};
    PageSaveResult save{};
    FormFieldReport forms{};

    [[nodiscard]] bool ok() const noexcept { return status == PageEditStatus::Ok; }
};

struct SplitOutput {
    std::string path;
    domain::pages::SplitChunk chunk{};
    int pageCount{0};
};

struct SplitResult {
    PageEditStatus status{PageEditStatus::Ok};
    std::string message;
    domain::pages::SplitPlan plan{};
    std::vector<SplitOutput> outputs;

    [[nodiscard]] bool ok() const noexcept { return status == PageEditStatus::Ok; }
};

// 「裁切至白邊」的偵測參數。
//
// 這件事沒有純結構的做法：白邊的定義是「畫出來是白的」，只能光柵化後掃描。
// CLAUDE.md 禁止整頁光柵化指的是檢視路徑的效能要求；這裡是一次性的離線分析，
// 且刻意壓在低解析度，與縮圖同性質。代價要講清楚：偵測精度就是一個像素所對應的
// 點數（頁寬 / maxEdgePixels），所以結果一律往外取整並可再加安全邊界。
struct ContentBoundsOptions {
    int maxEdgePixels{1000};
    // 0–255。任一通道低於此值就算「有內容」。預設 250 而不是 255，是為了容忍
    // 抗鋸齒在純白背景上留下的極淡灰階，否則掃描白邊會被雜訊擋住。
    int whiteThreshold{250};
    double marginPt{0.0};        // 偵測到的邊界再往外擴這麼多點
    bool includeAnnotations{false};
};

class PageEditor {
public:
    PageEditor();
    ~PageEditor();

    PageEditor(const PageEditor&) = delete;
    PageEditor& operator=(const PageEditor&) = delete;
    PageEditor(PageEditor&&) noexcept;
    PageEditor& operator=(PageEditor&&) noexcept;

    // 檔案整份讀進記憶體再交給 PDFium，理由同 save::ScopedDocument：不持有作業系統
    // 層的檔案鎖，就地存檔的原子更名才可能成功。
    [[nodiscard]] bool open(const std::string& path, const std::string& password = {});
    // 空白文件，合併與擷取的目標。沒有原始檔，因此只能 saveAsCopy。
    [[nodiscard]] bool createEmpty();
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] int pageCount() const;
    [[nodiscard]] const std::string& sourcePath() const noexcept;
    // 自開檔以來是否做過會改變頁面樹的操作。
    [[nodiscard]] bool hasStructuralChange() const noexcept;

    // ---- WBS 5.6 頁面管理 -------------------------------------------------

    PageEditResult apply(const domain::pages::PageOperation& operation);

    PageEditResult insertBlankPages(int atIndex, int count, double widthPt, double heightPt);
    PageEditResult deletePages(const std::vector<int>& pages);
    PageEditResult rotatePages(const std::vector<int>& pages, domain::pages::PageRotation rotation,
                               bool relative = true);
    PageEditResult movePages(const std::vector<int>& pages, int destinationIndex);
    PageEditResult duplicatePages(const std::vector<int>& pages, int destinationIndex);
    PageEditResult swapPages(int first, int second);
    PageEditResult reversePages();

    [[nodiscard]] std::optional<domain::pages::PageRotation> rotation(int pageIndex) const;
    [[nodiscard]] std::optional<domain::SizeF> pageSize(int pageIndex) const;

    // ---- WBS 5.8 裁切 -----------------------------------------------------

    // CropBox 會被夾進 MediaBox：PDF 規格要求前者落在後者之內，超出的部分在
    // 各家檢視器的行為並不一致，與其寫出一份「看情況」的檔案不如先夾好。
    PageEditResult setCropBox(int pageIndex, const domain::RectF& box);
    [[nodiscard]] std::optional<domain::RectF> cropBox(int pageIndex) const;
    [[nodiscard]] std::optional<domain::RectF> mediaBox(int pageIndex) const;

    // 掃描頁面內容，回傳非白像素的外接矩形（頁面座標，點）。整頁皆白時回傳 nullopt
    // ——那代表「沒有內容可以框」，把它當成空矩形去裁切會產出一份沒有頁面的文件。
    [[nodiscard]] std::optional<domain::RectF> detectContentBounds(
        int pageIndex, const ContentBoundsOptions& options = {}) const;

    // 裁切至白邊（PRD-PAGE-003）。整頁皆白的頁會被跳過而不是失敗；
    // 回傳值是實際被裁切的頁數。
    PageEditResult cropToContent(const std::vector<int>& pages,
                                 const ContentBoundsOptions& options = {},
                                 int* croppedPages = nullptr);

    // ---- 存檔 -------------------------------------------------------------

    // 增量儲存到 targetPath（可等於原路徑）。未經 open() 開檔（例如 createEmpty）
    // 時回報失敗而不是自動改走整份重寫——那個決定屬於呼叫端。
    [[nodiscard]] PageSaveResult save(const std::string& targetPath,
                                      const save::SaveOptions& options = {});
    [[nodiscard]] PageSaveResult saveAsCopy(const std::string& targetPath,
                                            const save::SaveOptions& options = {});

    // 每頁單獨輸出成一份 PDF 時的位元組數。依大小分割需要它，而唯一誠實的估法
    // 就是真的把每頁匯出一次量測——頁面共用資源時各頁加總會大於原檔，這是實情，
    // 不是誤差。頁數多時很慢，只在 SplitMode::MaxBytes 用得到。
    [[nodiscard]] std::vector<std::uint64_t> estimatePageBytes() const;

    // 內部使用：把手型別是 FPDF_DOCUMENT。與 PdfiumEngine::withDocument 同樣的理由
    // 以 void* 傳遞，僅供同屬引擎層的元件轉交給 save 子系統。
    [[nodiscard]] void* documentHandle() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- WBS 5.7 擷取／合併／分割 ---------------------------------------------
//
// 這三者的輸出都是**新文件**，沒有可保留的原始位元組，因此一律走 saveAsCopy
// （整份重寫）。這不是偷懶：對一份剛建立的文件談增量儲存沒有意義，而原檔的
// 既有簽章本來就不可能跟著被擷取出來的頁面走。

// 擷取指定頁另存新檔（PRD-PAGE-002）。pages 為 0 起算，順序即輸出順序，允許重複。
[[nodiscard]] AssemblyResult extractPages(const std::string& sourcePath,
                                          const std::vector<int>& pages,
                                          const std::string& targetPath,
                                          const std::string& password = {});

// 合併多份文件（PRD-PAGE-002）。sources 的順序即頁面順序。
[[nodiscard]] AssemblyResult mergeDocuments(const std::vector<std::string>& sourcePaths,
                                            const std::string& targetPath);

// 分割（PRD-PAGE-002）。targetPattern 內的 "{n}" 會被換成 1 起算的檔案序號；
// 沒有 "{n}" 時序號會插在副檔名之前。
[[nodiscard]] SplitResult splitDocument(const std::string& sourcePath,
                                        const domain::pages::SplitRule& rule,
                                        const std::string& targetPattern,
                                        const std::string& password = {});

// 供上層與測試共用的檔名產生規則，行為與 splitDocument 內部一致。
[[nodiscard]] std::string formatSplitPath(const std::string& pattern, int oneBasedIndex);

}  // namespace alioth::engine::pages
