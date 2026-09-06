#include "engine/bookmarks/toc_page_builder.h"

#include <algorithm>

#include "engine/bookmarks/destination_codec.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/page_object_editor.h"

namespace alioth::engine::bookmarks {

using domain::bookmarks::BookmarkLink;
using domain::bookmarks::BookmarkTree;
using domain::bookmarks::TocLayout;
using domain::bookmarks::TocLine;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfStream;

namespace {

// WinAnsiEncoding 可以直接輸出的位元組。非 ASCII 一律換成問號，
// 理由見標頭：CJK 需要內嵌字型，而那是待決策項。
[[nodiscard]] std::string toWinAnsi(const std::string& utf8, bool& degraded) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char u = static_cast<unsigned char>(utf8[i]);
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
            ++i;
            continue;
        }
        std::size_t length = 1;
        if ((u & 0xE0) == 0xC0) length = 2;
        else if ((u & 0xF0) == 0xE0) length = 3;
        else if ((u & 0xF8) == 0xF0) length = 4;
        out.push_back('?');
        degraded = true;
        i += std::min(length, utf8.size() - i);
    }
    return out;
}

void appendShowText(std::string& content, const std::string& fontResource, double sizePt, double x,
                    double y, const std::string& winAnsi) {
    if (winAnsi.empty()) return;
    content += "BT\n/";
    content += fontResource;
    content += ' ';
    content += objects::formatReal(sizePt);
    content += " Tf\n";
    content += objects::formatReal(x);
    content += ' ';
    content += objects::formatReal(y);
    content += " Td\n(";
    content += objects::escapeLiteralString(winAnsi);
    content += ") Tj\nET\n";
}

[[nodiscard]] PdfObject makeType1Font(const std::string& baseFont) {
    PdfDictionary font;
    font.set("Type", objects::makeName("Font"));
    font.set("Subtype", objects::makeName("Type1"));
    font.set("BaseFont", objects::makeName(baseFont));
    // 標準 14 字型不寫 /Encoding 時用的是字型內建編碼，Helvetica 的內建編碼
    // 在 0xA0 以上與 WinAnsi 不同；明確寫出來才有可預期的結果。
    font.set("Encoding", objects::makeName("WinAnsiEncoding"));
    return PdfObject{std::move(font)};
}

// 根 /Pages 節點的物件編號。
[[nodiscard]] int rootPagesObject(const objects::IncrementalAppender& appender) {
    const int catalogNumber = catalogObjectNumber(appender.source());
    if (catalogNumber <= 0) return 0;
    const PdfObject catalog = appender.currentObject(catalogNumber);
    const PdfDictionary* dict = catalog.asDictionary();
    if (dict == nullptr) return 0;
    const PdfObject* pages = dict->find("Pages");
    if (pages == nullptr || !pages->isRef()) return 0;
    return pages->asRef().number;
}

}  // namespace

TocBuildResult buildTableOfContents(objects::IncrementalAppender& appender, const BookmarkTree& tree,
                                    const TocBuildOptions& options) {
    TocBuildResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開檔";
        return result;
    }
    if (!objects::isStandard14Font(options.baseFont) ||
        !objects::isStandard14Font(options.headingFont)) {
        result.diagnostic = "目錄頁只支援標準 14 字型";
        return result;
    }

    const int pagesNumber = rootPagesObject(appender);
    if (pagesNumber <= 0) {
        result.diagnostic = "找不到頁面樹的根（catalog 缺少 /Pages）";
        return result;
    }

    const TocLayout layout = domain::bookmarks::layoutTableOfContents(tree, options.layout);
    if (layout.pages.empty()) {
        result.diagnostic = "版面是空的（頁面尺寸與邊界不合理，或沒有任何書籤）";
        return result;
    }

    const PageIndexMap pages(appender.source());

    // 字型物件兩支共用給所有目錄頁：每頁各建一份只會讓增量段變大，
    // 而 PRD-IO-001 的增量預算是 20 KB。
    const int bodyFontNumber = appender.allocateObject();
    appender.setObject(bodyFontNumber, makeType1Font(options.baseFont));
    const int headingFontNumber = appender.allocateObject();
    appender.setObject(headingFontNumber, makeType1Font(options.headingFont));

    const double dotWidth = options.leaderDotWidthPt > 0.0
                                ? options.leaderDotWidthPt
                                : domain::bookmarks::estimateTextWidthPt(".", options.layout.fontSizePt);

    for (const domain::bookmarks::TocPage& page : layout.pages) {
        std::string content;
        PdfArray annots;

        for (const TocLine& line : page.lines) {
            const std::string& fontResource = line.isHeading ? "F1" : "F0";
            appendShowText(content, fontResource, line.fontSizePt, line.xPt, line.baselineYPt,
                           toWinAnsi(line.text, result.degradedNonAscii));

            if (!line.pageLabel.empty()) {
                if (dotWidth > 0.0 && line.leaderEndXPt > line.leaderStartXPt) {
                    const int count =
                        static_cast<int>((line.leaderEndXPt - line.leaderStartXPt) / dotWidth);
                    if (count > 0 && options.layout.leader != '\0') {
                        appendShowText(content, "F0", line.fontSizePt, line.leaderStartXPt,
                                       line.baselineYPt,
                                       std::string(static_cast<std::size_t>(count),
                                                   options.layout.leader));
                    }
                }
                appendShowText(content, "F0", line.fontSizePt, line.pageLabelXPt, line.baselineYPt,
                               toWinAnsi(line.pageLabel, result.degradedNonAscii));
            }

            if (!options.createLinks || !line.destination.has_value()) continue;
            PdfObject destination;
            if (!encodeDestination(pages, *line.destination, destination)) continue;

            PdfDictionary link;
            link.set("Type", objects::makeName("Annot"));
            link.set("Subtype", objects::makeName("Link"));
            link.set("Rect", objects::makeNumberArray({line.rect.left, line.rect.bottom,
                                                       line.rect.right, line.rect.top}));
            // /Border [0 0 0] 而不是省略：省略時的預設是 [0 0 1]，也就是每個
            // 連結周圍都會有一條黑框，目錄頁看起來像被畫了格線。
            link.set("Border", objects::makeNumberArray({0.0, 0.0, 0.0}));
            link.set("Dest", std::move(destination));
            annots.push_back(PdfObject{std::move(link)});
            ++result.linkCount;
        }
        result.lineCount += page.lines.size();

        PdfStream stream;
        stream.data = std::move(content);
        const int contentNumber = appender.allocateObject();
        appender.setObject(contentNumber, PdfObject{std::move(stream)});

        PdfDictionary fonts;
        fonts.set("F0", objects::makeRef(bodyFontNumber));
        fonts.set("F1", objects::makeRef(headingFontNumber));
        PdfDictionary resources;
        resources.set("Font", PdfObject{std::move(fonts)});

        PdfDictionary pageDict;
        pageDict.set("Type", objects::makeName("Page"));
        pageDict.set("Parent", objects::makeRef(pagesNumber));
        pageDict.set("MediaBox",
                     objects::makeNumberArray({0.0, 0.0, layout.pageWidthPt, layout.pageHeightPt}));
        pageDict.set("Resources", PdfObject{std::move(resources)});
        pageDict.set("Contents", objects::makeRef(contentNumber));
        if (!annots.empty()) pageDict.set("Annots", PdfObject{std::move(annots)});

        const int pageNumber = appender.allocateObject();
        appender.setObject(pageNumber, PdfObject{std::move(pageDict)});
        result.pageObjects.push_back(pageNumber);
    }

    // 掛上頁面樹。/Count 是「這棵子樹底下的葉頁數」，不是 /Kids 的長度。
    PdfObject pagesObject = appender.currentObject(pagesNumber);
    PdfDictionary* pagesDict = pagesObject.asDictionary();
    if (pagesDict == nullptr) {
        result.diagnostic = "頁面樹的根不是字典";
        return result;
    }
    PdfObject* kidsEntry = pagesDict->find("Kids");
    if (kidsEntry == nullptr) {
        result.diagnostic = "頁面樹的根沒有 /Kids";
        return result;
    }

    // /Kids 可能是間接參照。改錯地方的症狀是新頁完全不出現，而檔案照樣打得開。
    int kidsHolder = pagesNumber;
    PdfObject kidsObject;
    PdfArray* kids = nullptr;
    if (kidsEntry->isRef()) {
        kidsHolder = kidsEntry->asRef().number;
        kidsObject = appender.currentObject(kidsHolder);
        kids = kidsObject.asArray();
    } else {
        kids = kidsEntry->asArray();
    }
    if (kids == nullptr) {
        result.diagnostic = "/Kids 不是陣列";
        return result;
    }

    PdfArray inserted;
    inserted.reserve(result.pageObjects.size());
    for (const int number : result.pageObjects) inserted.push_back(objects::makeRef(number));

    if (options.placement == TocPlacement::Front) {
        kids->insert(kids->begin(), inserted.begin(), inserted.end());
    } else {
        kids->insert(kids->end(), inserted.begin(), inserted.end());
    }

    if (kidsHolder != pagesNumber) {
        if (!appender.updateObject(kidsHolder, kidsObject)) {
            result.diagnostic = "無法更新 /Kids 陣列物件";
            return result;
        }
    }

    const PdfObject* count = pagesDict->find("Count");
    const std::int64_t previous =
        count != nullptr ? count->asInteger(static_cast<std::int64_t>(pages.pageCount()))
                         : static_cast<std::int64_t>(pages.pageCount());
    pagesDict->set("Count",
                   PdfObject{previous + static_cast<std::int64_t>(result.pageObjects.size())});
    if (!appender.updateObject(pagesNumber, pagesObject)) {
        result.diagnostic = "無法更新頁面樹的根";
        return result;
    }

    result.ok = true;
    return result;
}

LinkWriteResult writeBookmarkLinks(objects::IncrementalAppender& appender,
                                   const std::vector<BookmarkLink>& links) {
    LinkWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "appender 尚未開檔";
        return result;
    }

    const PageIndexMap pages(appender.source());
    for (const BookmarkLink& link : links) {
        PdfRef page{};
        if (!pages.refAt(link.pageIndex, page)) {
            ++result.skipped;
            continue;
        }
        PdfObject destination;
        if (!encodeDestination(pages, link.destination, destination)) {
            ++result.skipped;
            continue;
        }

        PdfDictionary annot;
        annot.set("Type", objects::makeName("Annot"));
        annot.set("Subtype", objects::makeName("Link"));
        annot.set("Rect", objects::makeNumberArray(
                              {link.rect.left, link.rect.bottom, link.rect.right, link.rect.top}));
        annot.set("Border", objects::makeNumberArray({0.0, 0.0, 0.0}));
        annot.set("Dest", std::move(destination));

        const int annotNumber = appender.allocateObject();
        appender.setObject(annotNumber, PdfObject{std::move(annot)});

        const objects::PageEditStatus status =
            objects::appendToPageArray(appender, page, "Annots", objects::makeRef(annotNumber));
        if (!status.ok) {
            result.diagnostic = status.diagnostic;
            return result;
        }
        ++result.written;
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::bookmarks
