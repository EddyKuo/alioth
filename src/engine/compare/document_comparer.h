#pragma once

// 文件比對的門面（PRD-CMP-001，WBS 5.12）。
//
// 執行緒：兩份文件各自持有獨立的 TextExtractor（各自一條專用執行緒與一份獨立的
// 文件把手），這是 SDD §1.1 唯一合法的並行形態。但本檔**刻意讓兩份文件依序擷取**，
// 不同時跑：`exceptions/EXC_20260905_RD_SA_parallel_search.md` 記錄了多個把手
// 同時開檔會漏頁的未解問題，而「比對結果靜默少一段」比「比對慢一倍」嚴重得多。
// 兩份不同檔案並行已被驗證可行，所以這裡的保守是可以在該例外結案後直接放寬的，
// 但在那之前不得加上任何額外並行。

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "domain/diff.h"
#include "domain/document.h"
#include "engine/compare/duplicate_pages.h"
#include "engine/compare/page_align.h"
#include "engine/compare/page_tokens.h"
#include "engine/compare/token_diff.h"

namespace alioth::engine::compare {

struct CompareOptions {
    AlignOptions align{};
    DiffLimits diff{};

    // 大小寫差異是否視為變更。預設視為變更：審閱情境下「Alioth」改成「ALIOTH」
    // 是一次真實的修改。
    bool ignoreCase{false};
};

struct CompareResult {
    domain::DocumentError error{domain::DocumentError::None};
    bool oldDocumentFailed{false};  // 錯誤來自哪一份，UI 才能指名道姓
    domain::DocumentDiff diff;

    [[nodiscard]] bool ok() const noexcept { return error == domain::DocumentError::None; }
};

// 純資料的比對：頁面對齊 + 頁內文字差異。不碰 PDFium，可單獨測試。
[[nodiscard]] domain::DocumentDiff comparePageTexts(std::span<const PageTokens> oldPages,
                                                    std::span<const PageTokens> newPages,
                                                    const CompareOptions& options = {});

// 端到端比對。兩份檔案依序以獨立的 TextExtractor 擷取文字後交給 comparePageTexts。
[[nodiscard]] CompareResult compareDocuments(const std::string& oldPath,
                                             const std::string& newPath,
                                             const CompareOptions& options = {},
                                             const std::string& oldPassword = {},
                                             const std::string& newPassword = {});

struct DuplicateScanResult {
    domain::DocumentError error{domain::DocumentError::None};
    std::vector<DuplicatePageGroup> groups;

    [[nodiscard]] bool ok() const noexcept { return error == domain::DocumentError::None; }
};

// 尋找重複頁面（PRD-CMP-002）。
//
// 文字來自 TextExtractor；頁面尺寸來自 PDF 物件層（/MediaBox 與 /Rotate），
// 不另開第二個 PDFium 把手——同一份檔案同時被兩個把手開啟，正是上述例外裡
// 尚未排除的可疑因素之一。
[[nodiscard]] DuplicateScanResult scanDuplicatePages(const std::string& path,
                                                     const DuplicateOptions& options = {},
                                                     const std::string& password = {});

}  // namespace alioth::engine::compare
