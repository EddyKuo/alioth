#include "engine/create/csv_to_pdf.h"

#include <utility>

namespace alioth::engine::create {

using domain::create::CsvImportOptions;
using domain::create::CsvPage;
using domain::create::CsvTableLayout;
using objects::PdfDictionary;
using objects::PdfObject;

namespace {

// 表格線與文字混在同一段內容串流裡：格線先畫（線寬 0.5pt，灰階 0.6），
// 文字後畫在線之上。文字一律靠左，垂直置中在儲存格內——這與大多數試算表
// 軟體匯出的預設對齊一致，數字欄靠右對齊屬於「更講究的排版」，
// CSV 本身不帶欄位型別資訊，本功能不猜測哪一欄是數字。
std::string makePageContent(const std::vector<std::string>& header,
                            const std::vector<std::vector<std::string>>& rows,
                            const CsvTableLayout& layout, const std::string& fontResource) {
    const std::size_t columnCount = layout.columnCount();
    double tableWidth = 0.0;
    for (const double w : layout.columnWidthsPt) tableWidth += w;

    const std::size_t totalRows = rows.size() + (header.empty() ? 0 : 1);
    const double tableHeight = static_cast<double>(totalRows) * layout.rowHeightPt;

    std::string content = "q\n0.6 0.6 0.6 RG\n0.5 w\n";

    // 水平線：totalRows + 1 條（含頂邊與底邊）。
    double y = layout.topY;
    for (std::size_t i = 0; i <= totalRows; ++i) {
        content += objects::formatReal(layout.leftX) + " " + objects::formatReal(y) + " m\n";
        content += objects::formatReal(layout.leftX + tableWidth) + " " + objects::formatReal(y) +
                   " l\nS\n";
        y -= layout.rowHeightPt;
    }
    // 垂直線：columnCount + 1 條。
    double x = layout.leftX;
    for (std::size_t c = 0; c <= columnCount; ++c) {
        content += objects::formatReal(x) + " " + objects::formatReal(layout.topY) + " m\n";
        content += objects::formatReal(x) + " " + objects::formatReal(layout.topY - tableHeight) +
                   " l\nS\n";
        if (c < columnCount) x += layout.columnWidthsPt[c];
    }
    content += "Q\n";

    content += "BT\n/";
    content += objects::escapeName(fontResource);
    content += ' ';
    content += objects::formatReal(layout.fontSize);
    content += " Tf\n0 0 0 rg\n";

    auto drawRow = [&](const std::vector<std::string>& cells, double rowTopY, bool bold) {
        (void)bold;  // 表頭目前與本文同字型；粗體需要第二個字型資源，
                     // 現階段的視覺區隔靠格線與位置（永遠是第一列），保持單字型簡單。
        double cellX = layout.leftX;
        const double baseline =
            rowTopY - (layout.rowHeightPt + layout.fontSize) / 2.0 + layout.fontSize * 0.15;
        for (std::size_t c = 0; c < columnCount && c < cells.size(); ++c) {
            if (!cells[c].empty()) {
                content += objects::formatReal(1.0) + " 0 0 1 " +
                           objects::formatReal(cellX + 4.0) + " " + objects::formatReal(baseline) +
                           " Tm\n";
                content += '(';
                content += objects::escapeLiteralString(cells[c]);
                content += ") Tj\n";
            }
            cellX += layout.columnWidthsPt[c];
        }
    };

    double rowTop = layout.topY;
    if (!header.empty()) {
        drawRow(header, rowTop, true);
        rowTop -= layout.rowHeightPt;
    }
    for (const std::vector<std::string>& row : rows) {
        drawRow(row, rowTop, false);
        rowTop -= layout.rowHeightPt;
    }
    content += "ET\n";
    return content;
}

}  // namespace

std::size_t appendCsvPages(PdfDocumentBuilder& builder, const CsvTableLayout& layout) {
    if (!layout.ok || layout.pages.empty()) return 0;

    const int fontNumber = builder.allocateObject();
    builder.setObject(fontNumber,
                      makeStandardFontDictionary(domain::create::baseFontName(layout.font)));
    const std::string fontResource = "F0";

    for (const CsvPage& page : layout.pages) {
        PdfDictionary fonts;
        fonts.set(fontResource, objects::makeRef(fontNumber));
        PdfDictionary resources;
        resources.set("Font", PdfObject{std::move(fonts)});
        builder.addPage(layout.pageWidthPt, layout.pageHeightPt,
                        makePageContent(layout.headerRow, page.rows, layout, fontResource),
                        std::move(resources));
    }
    return layout.pages.size();
}

CsvImportResult createPdfFromCsv(std::string_view csvText, const CsvImportOptions& options) {
    CsvImportResult result;

    const CsvTableLayout layout = domain::create::layoutCsvTable(std::string(csvText), options);
    if (!layout.ok) {
        result.diagnostic = layout.diagnostic;
        return result;
    }

    PdfDocumentBuilder builder;
    builder.setProducer("Alioth CSV import");
    if (appendCsvPages(builder, layout) == 0) {
        result.diagnostic = "版面沒有產生任何頁面";
        return result;
    }

    DocumentBuildResult built = builder.build();
    if (!built.ok) {
        result.diagnostic = built.diagnostic;
        return result;
    }

    result.ok = true;
    result.bytes = std::move(built.bytes);
    result.pageCount = layout.pageCount();
    result.columnCount = layout.columnCount();
    std::size_t rows = 0;
    for (const auto& page : layout.pages) rows += page.rows.size();
    result.rowCount = rows;
    return result;
}

}  // namespace alioth::engine::create
