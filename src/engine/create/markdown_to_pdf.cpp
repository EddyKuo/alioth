#include "engine/create/markdown_to_pdf.h"

#include <map>
#include <utility>

namespace alioth::engine::create {

using domain::create::MarkdownImportOptions;
using domain::create::MarkdownLayout;
using domain::create::MarkdownPage;
using domain::create::PlacedRule;
using domain::create::PlacedRun;
using domain::create::StandardFont;
using objects::PdfDictionary;
using objects::PdfObject;

namespace {

// 資源名稱由字型決定而不是由出現順序決定：同一份文件裡「粗體」在每一頁
// 都叫同一個名字，比對輸出時才不會被編號漂移干擾。
std::string resourceNameFor(StandardFont font) {
    switch (font) {
        case StandardFont::Helvetica:
            return "F0";
        case StandardFont::HelveticaBold:
            return "F1";
        case StandardFont::HelveticaOblique:
            return "F2";
        case StandardFont::HelveticaBoldOblique:
            return "F3";
        case StandardFont::Courier:
            return "F4";
        case StandardFont::CourierBold:
            return "F5";
        case StandardFont::CourierOblique:
            return "F6";
    }
    return "F0";
}

// 每頁只登記這一頁真的用到的字型。全部都登記也能用，但那會讓每一頁的
// /Resources 都掛七個字型物件，其中多數頁面一個都沒用到。
std::string makePageContent(const MarkdownPage& page, std::vector<StandardFont>& usedFonts) {
    std::string content;

    for (const PlacedRule& rule : page.rules) {
        content += objects::formatReal(rule.thicknessPt);
        content += " w\n";
        content += objects::formatReal(rule.xPt);
        content += ' ';
        content += objects::formatReal(rule.yPt);
        content += " m\n";
        content += objects::formatReal(rule.xPt + rule.widthPt);
        content += ' ';
        content += objects::formatReal(rule.yPt);
        content += " l\nS\n";
    }

    if (!page.runs.empty()) {
        content += "BT\n";
        StandardFont currentFont = page.runs.front().font;
        double currentSize = -1.0;
        bool fontSet = false;
        double lastX = 0.0;
        double lastY = 0.0;
        bool positioned = false;

        for (const PlacedRun& run : page.runs) {
            if (!fontSet || run.font != currentFont || run.fontSize != currentSize) {
                currentFont = run.font;
                currentSize = run.fontSize;
                fontSet = true;
                bool known = false;
                for (const StandardFont used : usedFonts) {
                    if (used == currentFont) known = true;
                }
                if (!known) usedFonts.push_back(currentFont);
                content += '/';
                content += objects::escapeName(resourceNameFor(currentFont));
                content += ' ';
                content += objects::formatReal(currentSize);
                content += " Tf\n";
            }
            // Td 是相對於前一次的文字行矩陣原點，因此這裡送的是差值。
            // 用絕對座標得改成 Tm，而 Tm 會一併重設字型大小以外的所有狀態，
            // 反而更容易出錯。
            const double dx = positioned ? run.xPt - lastX : run.xPt;
            const double dy = positioned ? run.baselineYPt - lastY : run.baselineYPt;
            content += objects::formatReal(dx);
            content += ' ';
            content += objects::formatReal(dy);
            content += " Td\n";
            lastX = run.xPt;
            lastY = run.baselineYPt;
            positioned = true;

            // escapeLiteralString 不含外層括號，序列化器才補；內容串流裡
            // 沒有序列化器，所以這兩個字元必須自己寫。少了它們 qpdf 會在
            // 字串起點報 "EOF while reading token"，而 PDFium 照樣開得起來。
            content += '(';
            content += objects::escapeLiteralString(run.text);
            content += ") Tj\n";
        }
        content += "ET\n";
    }

    return content;
}

}  // namespace

std::size_t appendMarkdownPages(PdfDocumentBuilder& builder, const MarkdownLayout& layout) {
    if (!layout.ok || layout.pages.empty()) return 0;

    std::map<int, int> fontObjects;  // StandardFont 序數 → 物件編號
    auto fontObject = [&](StandardFont font) {
        const int key = static_cast<int>(font);
        const auto found = fontObjects.find(key);
        if (found != fontObjects.end()) return found->second;
        const int number = builder.allocateObject();
        builder.setObject(number,
                          makeStandardFontDictionary(domain::create::baseFontName(font)));
        fontObjects.emplace(key, number);
        return number;
    };

    for (const MarkdownPage& page : layout.pages) {
        std::vector<StandardFont> usedFonts;
        const std::string content = makePageContent(page, usedFonts);

        PdfDictionary fonts;
        for (const StandardFont font : usedFonts) {
            fonts.set(resourceNameFor(font), objects::makeRef(fontObject(font)));
        }
        PdfDictionary resources;
        if (fonts.size() > 0) resources.set("Font", PdfObject{std::move(fonts)});

        builder.addPage(layout.pageWidthPt, layout.pageHeightPt, content, std::move(resources));
    }
    return layout.pages.size();
}

MarkdownImportResult createPdfFromMarkdown(std::string_view markdown,
                                           const MarkdownImportOptions& options) {
    MarkdownImportResult result;

    const MarkdownLayout layout = domain::create::layoutMarkdown(markdown, options);
    if (!layout.ok) {
        result.diagnostic = layout.diagnostic;
        result.nonAscii = !domain::create::scanAscii(markdown).ok;
        return result;
    }

    PdfDocumentBuilder builder;
    builder.setProducer("Alioth markdown import");
    if (appendMarkdownPages(builder, layout) == 0) {
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
    result.bookmarks = layout.bookmarks;
    return result;
}

}  // namespace alioth::engine::create
