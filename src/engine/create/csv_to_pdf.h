#pragma once

// 從 CSV 建立 PDF（PRD-IO-013 的 CSV 部分，WBS 15）。
//
// 版面計算全部在 domain::create::layoutCsvTable（欄寬、截斷、跨頁表頭），
// 這一層只負責把算好的格線與文字翻成內容串流，與 text_to_pdf.h /
// markdown_to_pdf.h 同一個分工原則。
//
// Email（.eml）匯入明確不做，見 domain/document_source.h 該節的說明。

#include <cstddef>
#include <string>
#include <string_view>

#include "domain/document_source.h"
#include "engine/create/pdf_document_builder.h"

namespace alioth::engine::create {

struct CsvImportResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
    std::size_t pageCount{0};
    std::size_t columnCount{0};
    std::size_t rowCount{0};
};

std::size_t appendCsvPages(PdfDocumentBuilder& builder, const domain::create::CsvTableLayout& layout);

[[nodiscard]] CsvImportResult createPdfFromCsv(
    std::string_view csvText, const domain::create::CsvImportOptions& options = {});

}  // namespace alioth::engine::create
