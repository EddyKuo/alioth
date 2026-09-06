#pragma once

// 從 Markdown 建立 PDF（PRD-IO-012，WBS 15）。
//
// 支援範圍：h1–h3（h4 以下降級成 h3）、段落、有序與無序清單（含巢狀）、
// 圍欄程式碼區塊、行內粗體／斜體／等寬、水平線、反斜線跳脫。
//
// 表格與圖片內嵌刻意不做。兩者都需要真正的版面引擎——表格要欄寬協商、
// 儲存格內換行、跨頁表頭重複；圖片要與文字流互相避讓並處理跨頁。那套機制
// 的成本高於本工作包其餘所有部分的總和，而 PRD 把 Markdown 匯入列為 S/R2，
// 且 §2.1 已經因為同一個理由（需自建排版層）排除了內文編輯。遇到表格語法
// 時不會靜默吞掉：那幾行會被當成一般段落原樣印出來，使用者看得見它沒被
// 當表格處理。
//
// 書籤的處理見 domain::create::BookmarkIntent 的註解：這一層只回傳意圖資料，
// 不相依 engine/bookmarks。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "domain/document_source.h"
#include "engine/create/pdf_document_builder.h"

namespace alioth::engine::create {

struct MarkdownImportResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
    std::size_t pageCount{0};

    // 標題的書籤意圖，依文件順序。呼叫端要建立 /Outlines 的話，把這份資料
    // 餵給 engine/bookmarks 的寫入器；不要書籤就什麼都不必做。
    std::vector<domain::create::BookmarkIntent> bookmarks;

    bool nonAscii{false};
};

std::size_t appendMarkdownPages(PdfDocumentBuilder& builder,
                                const domain::create::MarkdownLayout& layout);

[[nodiscard]] MarkdownImportResult createPdfFromMarkdown(
    std::string_view markdown, const domain::create::MarkdownImportOptions& options = {});

}  // namespace alioth::engine::create
