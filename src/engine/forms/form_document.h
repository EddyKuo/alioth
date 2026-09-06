#pragma once

// 表單子系統的門面（WBS 6.1–6.4，PRD-FORM-001 ~ 005）。
//
// 執行緒規則與 TextExtractor 相同、理由也相同：PDFium 非執行緒安全，
// 表單環境（FPDF_FORMHANDLE）又綁在它的文件把手上，所以本類別持有
// **自己的專用執行緒與獨立的文件把手**，不共用 PdfiumEngine 的把手。
// 共用就等於兩條執行緒碰同一份文件——那不是效能問題，是資料損毀。
//
// 代價是同一份檔案被開啟兩次。這是 SDD §1.1 已經接受的取捨：
// 檔案唯讀開啟，重複開啟的成本遠低於共用把手的正確性風險。
//
// 所有操作皆為非同步，回呼在表單執行緒上被呼叫；呼叫端必須自行排回自己的執行緒。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/document.h"
#include "domain/geometry.h"
#include "engine/forms/form_data.h"
#include "engine/forms/form_environment.h"
#include "engine/forms/form_types.h"
#include "engine/pixel_buffer.h"

namespace alioth::engine::forms {

struct FormFillResult {
    bool ok{false};
    std::string error;  // 繁體中文；失敗一律有原因，不得只回 false（IL-4）
};

// 表單環境的事件橋接快照。UI 用它決定重畫哪一塊、是否顯示「已修改」；
// 測試用它確認填寫真的觸發了外觀更新，而不是只改了資料。
struct FormEventSnapshot {
    std::int32_t changeCount{0};
    std::vector<InvalidatedRegion> invalidations;
    std::vector<BlockedNavigation> blockedNavigations;
};

class FormDocument {
public:
    FormDocument();
    ~FormDocument();

    FormDocument(const FormDocument&) = delete;
    FormDocument& operator=(const FormDocument&) = delete;

    void open(std::string path, std::string password,
              std::function<void(domain::DocumentError)> callback);
    void close(std::function<void()> callback = {});

    [[nodiscard]] std::int32_t pageCount() const noexcept;

    // 辨識表單（PRD-FORM-005）。無表單文件必須回報 formType == None、
    // fieldCount == 0，不得因為文件裡有 /Annots 就誤報有表單。
    void identify(std::function<void(FormIdentification)> callback);

    // XFA 偵測與降級提示（PRD-FORM-003）。
    void xfaReport(std::function<void(XfaReport)> callback);

    void fields(std::int32_t pageIndex, std::function<void(std::vector<FormFieldInfo>)> callback);
    void allFields(std::function<void(std::vector<FormFieldInfo>)> callback);

    // 命中測試（頁面空間座標）。未命中回傳 nullopt。
    void fieldAt(std::int32_t pageIndex, domain::PointF pagePoint,
                 std::function<void(std::optional<FormFieldInfo>)> callback);

    // 以下寫入操作一律以 (pageIndex, annotIndex) 定位而不是欄位名：
    // 單選群組的多個 widget 共用同一個名字，用名字定位會改到錯的按鈕。
    void setTextValue(std::int32_t pageIndex, std::int32_t annotIndex, std::string value,
                      std::function<void(FormFillResult)> callback);
    void setChecked(std::int32_t pageIndex, std::int32_t annotIndex, bool checked,
                    std::function<void(FormFillResult)> callback);
    void setSelectedIndices(std::int32_t pageIndex, std::int32_t annotIndex,
                            std::vector<std::int32_t> indices,
                            std::function<void(FormFillResult)> callback);

    // 原始事件橋接（WBS 6.1）。呈現層的滑鼠與鍵盤事件直接轉進來，
    // 上面那組 setXxx 只是它們的常用組合。
    void mouseDown(std::int32_t pageIndex, domain::PointF pagePoint, int modifiers,
                   std::function<void(bool)> callback = {});
    void mouseUp(std::int32_t pageIndex, domain::PointF pagePoint, int modifiers,
                 std::function<void(bool)> callback = {});
    void typeText(std::int32_t pageIndex, std::string utf8, int modifiers,
                  std::function<void(bool)> callback = {});
    void killFocus(std::function<void(bool)> callback = {});

    // 欄位高亮（PRD-FORM-004）。type 為 nullopt 代表套用到所有欄位型別。
    void setFieldHighlight(std::optional<FormFieldType> type, std::uint32_t rgb,
                           std::uint8_t alpha, std::function<void()> callback = {});
    void clearFieldHighlight(std::function<void()> callback = {});

    // 匯出／匯入／重設／攤平（PRD-FORM-002）。
    void exportData(FormDataFormat format, std::string sourcePath,
                    std::function<void(std::string)> callback);
    void importData(std::string text, FormDataFormat format,
                    std::function<void(FormDataImport)> callback);
    void resetForm(std::function<void(FormFillResult)> callback);
    void flatten(std::function<void(FormFillResult)> callback);

    // 攤平後必須另存新檔才有意義：攤平是破壞性的，不能就地增量儲存
    // （增量儲存的前提是純附加，而攤平改寫了頁面內容串流）。
    void saveCopy(std::string path, std::function<void(FormFillResult)> callback);

    // 頁面渲染。includeFormOverlay 為真時在頁面內容之後呼叫 FPDF_FFLDraw，
    // 這是表單 widget 互動外觀（焦點框、插字游標、高亮）唯一的來源。
    void renderPage(std::int32_t pageIndex, std::int32_t widthPx, std::int32_t heightPx,
                    bool includeFormOverlay, std::function<void(PixelBufferPtr)> callback);

    void eventSnapshot(std::function<void(FormEventSnapshot)> callback);
    void clearEvents(std::function<void()> callback = {});

    // 同步等待佇列清空，僅供測試使用。
    void waitForIdle();

    [[nodiscard]] std::size_t pendingTaskCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::forms
