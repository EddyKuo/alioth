#pragma once

// 表單填寫環境與事件橋接（WBS 6.1，PRD-FORM-001）。
//
// FPDF_FORMFILLINFO 是 PDFium 反過來呼叫宿主的介面：重繪、游標、計時器、
// 取頁、跳頁、開網址都由它回呼。這個類別把那組 C 回呼收斂成一個物件，
// 並且把「PDFium 想做但我們不准它做的事」（開啟網址、執行具名動作）
// 記錄下來交給上層決定，而不是直接執行——PDF 是不可信任輸入。
//
// XFA 立場（PRD-FORM-003、CLAUDE.md §範圍邊界）：
// 本建置的 pdfium 已關閉 XFA，這裡再多做兩件事把立場寫死——
// xfa_disabled 明確設為 TRUE，且**永不呼叫 FPDF_LoadXFA**。
// 「建置已關掉」不該是唯一的防線；換一份 dll 就會破功。
//
// JavaScript 立場（PRD §8.2）：m_pJsPlatform 恆為 nullptr。
// 表單計算改用受限運算式引擎（PRD-FORM-022，R2），不引入 JS 引擎。
//
// 執行緒規則：本類別的所有成員函式都必須在擁有該文件把手的表單執行緒上呼叫。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::engine::forms {

// PDFium 要求重繪的區域。座標是頁面空間（點）。
// 累積起來而不是立刻重繪，讓呼叫端可以合併成一次 UI 更新。
struct InvalidatedRegion {
    std::int32_t pageIndex{-1};
    domain::RectF rectPt{};
};

// 被攔下來的導覽請求。CLAUDE.md 要求開啟網址前需確認、一律禁止 Launch Action，
// 所以這裡只記錄不執行；要不要開由呈現層問過使用者再決定。
struct BlockedNavigation {
    enum class Kind { Uri, NamedAction, GoTo };
    Kind kind{Kind::Uri};
    std::string payload;       // URI 或具名動作名稱
    std::int32_t pageIndex{-1};  // 僅 GoTo 有意義
};

class FormEnvironment {
public:
    // 由呼叫端提供頁面把手的取得與反查。表單環境不自己管理頁面生命週期——
    // 頁面快取屬於文件層，兩邊各管一份必然會出現一邊已關、另一邊還在用的懸置把手。
    using PageLoader = std::function<void*(std::int32_t)>;      // index -> FPDF_PAGE
    using PageIndexResolver = std::function<std::int32_t(void*)>;  // FPDF_PAGE -> index

    FormEnvironment();
    ~FormEnvironment();

    FormEnvironment(const FormEnvironment&) = delete;
    FormEnvironment& operator=(const FormEnvironment&) = delete;

    // document 的實際型別是 FPDF_DOCUMENT。成功後 handle() 可用。
    [[nodiscard]] bool attach(void* document, PageLoader loader, PageIndexResolver resolver);
    void detach();

    [[nodiscard]] bool valid() const noexcept;
    // 實際型別 FPDF_FORMHANDLE；只給同屬引擎轉接層的元件使用。
    [[nodiscard]] void* handle() const noexcept;

    // 欄位高亮（PRD-FORM-004）。fieldType 為 -1 代表套用到所有型別。
    void setFieldHighlight(std::int32_t fieldType, std::uint32_t rgb, std::uint8_t alpha);
    void removeFieldHighlight();
    [[nodiscard]] bool highlightEnabled() const noexcept;

    // 事件橋接的觀測點。UI 用它決定重畫哪一塊；測試用它確認外觀真的被更新過。
    [[nodiscard]] const std::vector<InvalidatedRegion>& invalidations() const noexcept;
    void clearInvalidations();

    // FFI_OnChange 次數。表單「已修改未儲存」狀態的唯一來源。
    [[nodiscard]] std::int32_t changeCount() const noexcept;
    void resetChangeCount();

    [[nodiscard]] const std::vector<BlockedNavigation>& blockedNavigations() const noexcept;
    void clearBlockedNavigations();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::forms
