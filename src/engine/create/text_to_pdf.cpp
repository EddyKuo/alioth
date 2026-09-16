#include "engine/create/text_to_pdf.h"

#include <utility>
// toUcs4() 回傳 QList<uint>，而 QStringDecoder 只前置宣告它。少了這個 include
// 的錯誤訊息是「使用未定義類型 QList<uint>」，看起來像範本錯而不是缺標頭。
#include <QList>
#include <QStringDecoder>
#include "engine/fonts/cjk_font_library.h"
#include "engine/fonts/cid_font_writer.h"
#include "engine/fonts/text_runs.h"

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
        for (const auto& run : fonts::splitTextRuns(lines[i])) {
            content += "/" + (run.cjk ? std::string("CJK") : fontResource) + " " +
                       objects::formatReal(layout.fontSize) + " Tf\n(";
            content += run.cjk ? run.bytes : objects::escapeLiteralString(run.bytes);
            content += ") Tj\n";
        }
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
    std::set<char32_t> codepoints;
    for (const auto& page : layout.pages) {
        for (const auto& line : page) {
            for (char32_t cp : fonts::decodeUtf8(line)) {
                if (fonts::needsCjkFont(cp)) codepoints.insert(cp);
            }
        }
    }
    int cjkFont = 0;
    if (!codepoints.empty()) {
        auto& library = fonts::CjkFontLibrary::instance();
        const auto subset = library.subsetFor(codepoints);
        const auto embedded = fonts::embedSubsetFont([&builder](objects::PdfObject object) {
            const int number = builder.allocateObject();
            builder.setObject(number, std::move(object));
            return number;
        }, subset, library.baseName());
        if (!embedded.ok) return 0;
        cjkFont = embedded.fontObject;
    }
    for (const std::vector<std::string>& lines : layout.pages) {
        PdfDictionary fonts;
        fonts.set(fontResource, objects::makeRef(fontNumber));
        if (cjkFont != 0) fonts.set("CJK", objects::makeRef(cjkFont));
        PdfDictionary resources;
        resources.set("Font", PdfObject{std::move(fonts)});
        builder.addPage(layout.pageWidthPt, layout.pageHeightPt,
                        makePageContent(lines, layout, fontResource), std::move(resources));
    }
    return layout.pages.size();
}

TextImportResult createPdfFromPlainText(std::string_view text, const TextImportOptions& options) {
    TextImportResult result;

    // 只掃一次。layoutPlainText 內部也會 scanAscii，所以純 ASCII 的輸入由它
    // 負責，非 ASCII 的輸入則不再進去掃第二次——那是整份文字的 O(N) 重走。
    const domain::create::AsciiScan scan = domain::create::scanAscii(text);
    result.nonAscii = !scan.ok;

    domain::create::TextLayout layout;
    if (scan.ok) {
        layout = domain::create::layoutPlainText(text, options);
    } else {
        // Validate before decoding: malformed UTF-8 must never silently lose bytes.
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString unicode = decoder(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())));
        if (decoder.hasError()) {
            result.diagnostic = "文字不是有效的 UTF-8";
            return result;
        }
        // 版面幾何（紙張、邊距、行距、首行基線）與 ASCII 完全共用，只有
        // 字寬的來源不同，所以先讓 domain 用空字串把幾何算出來。
        layout = domain::create::layoutPlainText("", options);
        if (!layout.ok) { result.diagnostic = layout.diagnostic; return result; }
        auto& library = fonts::CjkFontLibrary::instance();
        for (char32_t cp : unicode.toUcs4()) {
            if ((cp < 32 && cp != '\n' && cp != '\r' && cp != '\t' && cp != '\f') || cp == 127) {
                result.diagnostic = "文字含無法顯示的控制字元";
                return result;
            }
            if (cp >= 128 && (!library.available() || library.glyphFor(cp) == 0)) {
                result.diagnostic = "內嵌字型缺少字形 U+" + QString::number(cp, 16).toStdString();
                return result;
            }
        }
        layout.pages.clear();
        const double width = layout.pageWidthPt - 2 * layout.leftX;
        const int tabWidth = std::max(1, options.tabWidth);
        // 分頁、\f 分段與尾端空行的規則走 domain 那一份，這裡只提供斷行：
        // CJK 沒有空白可斷，逐字元量寬即可，但版面規則不該因此分岔。
        if (!domain::create::detail::paginate(
                text, domain::create::detail::linesPerPage(layout), layout.pages,
                [&](const std::string& raw, std::vector<std::string>& out) {
                    QString line;
                    double used = 0;
                    int column = 0;
                    for (char32_t cp : QString::fromUtf8(raw).toUcs4()) {
                        const int repeats = cp == '\t' ? tabWidth - column % tabWidth : 1;
                        if (cp == '\t') cp = ' ';
                        const double advance = cp >= 128 ? library.advanceFor(cp) * layout.fontSize / 1000.0 :
                            domain::create::measureText(layout.font, std::string(1, static_cast<char>(cp)), layout.fontSize);
                        if (advance > width) {
                            result.diagnostic = "版心無法容納單一字形";
                            return false;
                        }
                        for (int n = 0; n < repeats; ++n) {
                            if (used + advance > width && !line.isEmpty()) {
                                out.push_back(line.toUtf8().toStdString());
                                line.clear(); used = 0; column = 0;
                            }
                            line += QString::fromUcs4(&cp, 1);
                            used += advance;
                            ++column;
                        }
                    }
                    out.push_back(line.toUtf8().toStdString());
                    return true;
                })) {
            return result;
        }
    }
    if (!layout.ok) {
        result.diagnostic = layout.diagnostic;
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
    result.nonAscii = false;
    result.bytes = std::move(built.bytes);
    result.pageCount = layout.pageCount();
    result.lineCount = layout.lineCount();
    return result;
}

}  // namespace alioth::engine::create
