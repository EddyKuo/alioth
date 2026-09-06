#include "engine/create/text_to_pdf.h"

#include <utility>

namespace alioth::engine::create {

using domain::create::TextImportOptions;
using domain::create::TextLayout;
using objects::PdfDictionary;
using objects::PdfObject;

namespace {

// 一頁的內容串流。整頁走同一個 BT/ET，行間用 TL + T* 前進：
// 每一行各自 Td 也可以，但那樣行距會散落在數十個數字裡，日後要調整
// 就得同時改對每一個，而漏掉其中一個的症狀是某幾行莫名擠在一起。
std::string makePageContent(const std::vector<std::string>& lines, const TextLayout& layout,
                            const std::string& fontResource) {
    std::string content = "BT\n/";
    content += objects::escapeName(fontResource);
    content += ' ';
    content += objects::formatReal(layout.fontSize);
    content += " Tf\n";
    content += objects::formatReal(layout.leading);
    content += " TL\n";
    content += objects::formatReal(layout.leftX);
    content += ' ';
    content += objects::formatReal(layout.firstBaselineY);
    content += " Td\n";

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) content += "T*\n";
        if (lines[i].empty()) continue;
        // escapeLiteralString 只做跳脫，不含外層括號（序列化器才補上它們）。
        // 少了這兩個字元，qpdf 會在字串起點報 "EOF while reading token"，
        // 而 PDFium 照樣開得起來——正是需要外部裁判的那類缺陷。
        content += '(';
        content += objects::escapeLiteralString(lines[i]);
        content += ") Tj\n";
    }
    content += "ET\n";
    return content;
}

}  // namespace

std::size_t appendTextPages(PdfDocumentBuilder& builder, const TextLayout& layout) {
    if (!layout.ok || layout.pages.empty()) return 0;

    // 字型物件只建一次並由所有頁面共用。每頁各自一份也能用，但一份 500 頁
    // 的文字檔就會多出 500 個內容完全相同的字典。
    const int fontNumber = builder.allocateObject();
    builder.setObject(fontNumber,
                      makeStandardFontDictionary(domain::create::baseFontName(layout.font)));

    const std::string fontResource = "F0";
    for (const std::vector<std::string>& lines : layout.pages) {
        PdfDictionary fonts;
        fonts.set(fontResource, objects::makeRef(fontNumber));
        PdfDictionary resources;
        resources.set("Font", PdfObject{std::move(fonts)});
        builder.addPage(layout.pageWidthPt, layout.pageHeightPt,
                        makePageContent(lines, layout, fontResource), std::move(resources));
    }
    return layout.pages.size();
}

TextImportResult createPdfFromPlainText(std::string_view text, const TextImportOptions& options) {
    TextImportResult result;

    const domain::create::TextLayout layout = domain::create::layoutPlainText(text, options);
    if (!layout.ok) {
        result.diagnostic = layout.diagnostic;
        result.nonAscii = !domain::create::scanAscii(text).ok;
        return result;
    }

    PdfDocumentBuilder builder;
    builder.setProducer("Alioth text import");
    if (appendTextPages(builder, layout) == 0) {
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
    result.lineCount = layout.lineCount();
    return result;
}

}  // namespace alioth::engine::create
