#include "engine/redaction/redaction_applier.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "engine/fonts/cid_font_writer.h"
#include "engine/fonts/cjk_font_library.h"
#include "engine/fonts/text_runs.h"
#include "engine/objects/pdf_parser.h"
#include "engine/redaction/content_redactor.h"
#include "engine/redaction/pdf_document_rewriter.h"
#include "engine/redaction/redaction_marks.h"

namespace alioth::engine::redaction {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfStream;

// 覆蓋矩形用的字型資源名稱。刻意帶前綴，避免與原檔既有的 /F1 之類撞名——
// 撞名的後果是把原檔的字型換掉，整頁的文字變成另一種字體。
constexpr const char* kOverlayFontResource = "AliothRedactHelv";
// 內嵌 CJK 子集的資源名稱（ADR-007）。與內容串流寫出的名稱必須一致——
// 對不上的話覆蓋文字的中文整段消失，而**塗黑的覆蓋文字消失看起來像塗黑成功了**，
// 使用者不會發現標示不見了。這比一般的「字沒畫出來」更嚴重。
constexpr const char* kCjkFontResource = "AliothRedactCJK";

std::string formatReal(double value) { return objects::formatReal(value); }

struct PageContent {
    std::vector<int> streamObjects{};
    std::string decoded{};
};

// 收集頁面的內容串流並解碼成一段連續位元組。
//
// /Contents 是陣列時，各段在語意上等同單一串流（§7.8.2），因此合併後編輯是對的；
// 反過來逐段獨立編輯才是錯的：圖形狀態會跨段延續，逐段解讀會算錯 CTM。
bool collectPageContent(PdfDocumentRewriter& document, const PdfDictionary& page,
                        PageContent& out, std::string& diagnostic) {
    const PdfObject* contents = page.find("Contents");
    if (contents == nullptr) return true;

    std::vector<PdfObject> entries;
    if (contents->isRef()) {
        const PdfObject* target = document.object(contents->asRef().number);
        if (target == nullptr) return true;
        if (target->asArray() != nullptr) {
            for (const PdfObject& item : *target->asArray()) entries.push_back(item);
        } else {
            entries.push_back(*contents);
        }
    } else if (const PdfArray* array = contents->asArray()) {
        for (const PdfObject& item : *array) entries.push_back(item);
    }

    for (const PdfObject& entry : entries) {
        if (!entry.isRef()) continue;
        const int number = entry.asRef().number;
        const PdfObject* target = document.object(number);
        if (target == nullptr) continue;
        const PdfStream* stream = target->asStream();
        if (stream == nullptr) continue;

        // 被兩頁共用的內容串流改一邊等於改兩邊，而另一頁並沒有要求塗黑。
        // 這種檔案存在（版面完全相同的重複頁），必須明確失敗而不是二選一。
        if (document.referenceCount(number) > 1) {
            diagnostic = "內容串流物件 " + std::to_string(number) +
                         " 被多個頁面共用，無法在不影響其他頁面的前提下塗黑";
            return false;
        }

        const objects::DecodeResult decoded =
            objects::decodeStream(*stream, [&document](const objects::PdfRef& ref) {
                const PdfObject* found = document.object(ref.number);
                return found == nullptr ? PdfObject{} : *found;
            });
        if (!decoded.ok) {
            diagnostic = "內容串流物件 " + std::to_string(number) +
                         " 的濾鏡無法解開，因此無法確認要移除哪些內容：" + decoded.diagnostic;
            return false;
        }
        out.streamObjects.push_back(number);
        if (!out.decoded.empty()) out.decoded += '\n';
        out.decoded += decoded.data;
    }
    return true;
}

// 覆蓋矩形的繪圖指令。安全性不來自這裡，它只是讓人看得出來這塊被處理過。
std::string overlayContent(const std::vector<domain::RectF>& areas,
                           const domain::RedactionMark& mark, bool withText) {
    std::string content = "q\n";
    content += formatReal(mark.fillColor.r) + ' ' + formatReal(mark.fillColor.g) + ' ' +
               formatReal(mark.fillColor.b) + " rg\n";
    for (const domain::RectF& raw : areas) {
        const domain::RectF area = raw.normalized();
        content += formatReal(area.left) + ' ' + formatReal(area.bottom) + ' ' +
                   formatReal(area.width()) + ' ' + formatReal(area.height()) + " re\n";
    }
    content += "f\n";

    if (withText && mark.overlayText.has_value() && !mark.overlayText->empty()) {
        // 深色底配白字、淺色底配黑字。用固定顏色會在某些填色下變成看不見的文字，
        // 而使用者會以為覆蓋文字沒有寫進去。
        const double luminance =
            0.299 * mark.fillColor.r + 0.587 * mark.fillColor.g + 0.114 * mark.fillColor.b;
        content += luminance < 0.5 ? "1 1 1 rg\n" : "0 0 0 rg\n";
        const domain::RectF box = mark.boundingBox();
        const double size = mark.overlayFontSize > 0.0 ? mark.overlayFontSize : 8.0;
        const double baseline = box.bottom + std::max(0.0, (box.height() - size) / 2.0);
        content += "BT\n";
        content += formatReal(box.left + 2.0) + ' ' + formatReal(baseline) + " Td\n";
        // 拉丁與 CJK 的字型與編碼都不同，必須分段畫（ADR-007）。整段用同一個
        // 字型的話，不是中文變亂碼就是拉丁字被當成雙位元組讀掉。
        for (const fonts::TextRun& run : fonts::splitTextRuns(*mark.overlayText)) {
            content += "/";
            content += run.cjk ? kCjkFontResource : kOverlayFontResource;
            content += ' ' + formatReal(size) + " Tf\n";
            if (run.cjk) {
                // run.bytes 已是 Identity-H 編碼並跳脫過，不可再跳脫一次——
                // 二次跳脫會把反斜線本身變成資料，整組 GID 因此偏掉。
                content += "(" + run.bytes + ") Tj\n";
            } else {
                content += objects::serialize(objects::makeLiteralString(run.bytes));
                content += " Tj\n";
            }
        }
        content += "ET\n";
    }
    content += "Q\n";
    return content;
}

PdfObject overlayFontObject() {
    PdfDictionary font;
    font.set("Type", objects::makeName("Font"));
    font.set("Subtype", objects::makeName("Type1"));
    font.set("BaseFont", objects::makeName("Helvetica"));
    font.set("Encoding", objects::makeName("WinAnsiEncoding"));
    return PdfObject{std::move(font)};
}

bool isAscii(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// 刪掉落在塗黑區域內的註解。
//
// 這是 Redaction 最常被忘記的一塊：便利貼、文字方塊、彈出視窗的內容都不在
// 內容串流裡，只處理內容串流會讓它們原封不動留在檔案裡。
int removeOverlappingAnnotations(PdfDocumentRewriter& document, int pageObject,
                                 const std::vector<domain::RectF>& areas) {
    PdfArray* annots = unsharedDictionaryArray(document, pageObject, "Annots");
    if (annots == nullptr) return 0;

    PdfArray kept;
    int removed = 0;
    for (const PdfObject& entry : *annots) {
        const PdfObject annotation = document.resolve(entry);
        const PdfDictionary* dict = annotation.asDictionary();
        if (dict == nullptr) {
            kept.push_back(entry);
            continue;
        }
        const PdfObject* rectValue = dict->find("Rect");
        const PdfObject rect = rectValue == nullptr ? PdfObject{} : document.resolve(*rectValue);
        const PdfArray* array = rect.asArray();
        if (array == nullptr || array->size() < 4) {
            kept.push_back(entry);
            continue;
        }
        const domain::RectF box = domain::RectF{(*array)[0].asNumber(), (*array)[1].asNumber(),
                                                (*array)[2].asNumber(), (*array)[3].asNumber()}
                                      .normalized();
        if (domain::overlapsAny(areas, box)) {
            ++removed;
            continue;
        }
        kept.push_back(entry);
    }
    if (removed > 0) *annots = std::move(kept);
    return removed;
}

struct PageJob {
    std::vector<domain::RectF> areas{};
    std::vector<domain::RedactionMark> marks{};
};

bool applyToPage(PdfDocumentRewriter& document, int pageIndex, const PageJob& job,
                 const domain::RedactionPlan& plan, ApplyStats& stats, std::string& diagnostic) {
    objects::PdfRef pageRef{};
    if (!document.pageRef(pageIndex, pageRef)) {
        diagnostic = "頁碼超出範圍：" + std::to_string(pageIndex);
        return false;
    }
    const int pageObject = pageRef.number;
    const PdfObject* page = document.object(pageObject);
    if (page == nullptr || page->asDictionary() == nullptr) {
        diagnostic = "頁面物件不是字典：" + std::to_string(pageObject);
        return false;
    }

    PageContent content;
    if (!collectPageContent(document, *page->asDictionary(), content, diagnostic)) return false;

    const PdfObject resources = document.inheritedPageAttribute(pageRef, "Resources");

    ContentRedactionRequest request;
    request.areas = job.areas;
    request.textPolicy = plan.textPolicy();
    request.imagePolicy = plan.imagePolicy();

    const ContentRedactionResult edited = redactContentStream(
        document, content.decoded, resources, identityMatrix(), request);
    if (!edited.ok) {
        diagnostic = edited.diagnostic;
        return false;
    }

    std::string finalContent = edited.content;
    if (plan.drawsOverlay()) {
        bool needsFont = false;
        // 整頁的覆蓋文字共用一份子集。逐個標記各嵌一份，會讓一頁上十個
        // 塗黑標記內嵌十份幾乎相同的字型。
        std::set<char32_t> cjkCodepoints;
        for (const domain::RedactionMark& mark : job.marks) {
            const bool withText = mark.overlayText.has_value() && !mark.overlayText->empty();
            if (withText && !isAscii(*mark.overlayText)) {
                // ADR-007 之後 CJK 走內嵌子集。但畫不出來仍然要明確失敗——
                // 塗黑的覆蓋文字若缺字或整段消失，畫面上看起來就像「塗黑成功了」，
                // 使用者不會發現標示不見了。這比一般的漏字嚴重。
                auto& library = fonts::CjkFontLibrary::instance();
                if (!library.available()) {
                    diagnostic = "覆蓋文字含非 ASCII 字元，但沒有可用的 CJK 字型。" +
                                 library.diagnostic();
                    return false;
                }
                for (const char32_t codepoint : fonts::decodeUtf8(*mark.overlayText)) {
                    if (codepoint < 128) continue;
                    if (library.glyphFor(codepoint) == 0) {
                        char buffer[32];
                        std::snprintf(buffer, sizeof(buffer), "U+%04X",
                                      static_cast<unsigned>(codepoint));
                        diagnostic = std::string("覆蓋文字有字型缺少的字：") + buffer;
                        return false;
                    }
                    cjkCodepoints.insert(codepoint);
                }
            }
            needsFont = needsFont || withText;
            if (!finalContent.empty() && finalContent.back() != '\n') finalContent += '\n';
            std::vector<domain::RectF> areas;
            for (const domain::RectF& area : mark.areas) areas.push_back(area.normalized());
            finalContent += overlayContent(areas, mark, withText);
        }
        if (needsFont &&
            !setResourceEntry(document, pageObject, "Font", kOverlayFontResource,
                              objects::makeRef(document.addObject(overlayFontObject())))) {
            diagnostic = "無法為覆蓋文字登記字型資源";
            return false;
        }
        if (!cjkCodepoints.empty()) {
            auto& library = fonts::CjkFontLibrary::instance();
            const fonts::SubsetResult subset = library.subsetFor(cjkCodepoints);
            if (!subset.ok) {
                diagnostic = "CJK 字型子集化失敗：" + subset.diagnostic;
                return false;
            }
            // 塗黑走整份重寫（不是增量附加），因此用回呼版本的內嵌函式。
            // 明確建成 ObjectSink：多載解析看到 lambda 時會先試 IncrementalAppender
            // 那一版，而 lambda 轉不成它，於是整個呼叫失敗而不是選另一版。
            const fonts::ObjectSink sink = [&document](objects::PdfObject object) {
                return document.addObject(std::move(object));
            };
            const fonts::EmbeddedFontResult embedded =
                fonts::embedSubsetFont(sink, subset, library.baseName());
            if (!embedded.ok) {
                diagnostic = "無法內嵌 CJK 字型：" + embedded.diagnostic;
                return false;
            }
            if (!setResourceEntry(document, pageObject, "Font", kCjkFontResource,
                                  objects::makeRef(embedded.fontObject))) {
                diagnostic = "無法為覆蓋文字登記 CJK 字型資源";
                return false;
            }
        }
    }

    if (!edited.removedXObjectNames.empty() &&
        !removeResourceEntries(document, pageObject, "XObject", edited.removedXObjectNames)) {
        diagnostic = "無法從資源字典移除被塗黑的影像";
        return false;
    }

    PdfDictionary contentDict;
    const int contentObject =
        document.addObject(PdfObject{PdfStream{std::move(contentDict), std::move(finalContent)}});

    // 舊的內容串流物件在這一步之後就不再可達，因此不會被寫進輸出檔案。
    // 這正是「原文不得出現在檔案任何位置」的實作點。
    PdfObject* mutablePage = document.object(pageObject);
    mutablePage->asDictionary()->set("Contents", objects::makeRef(contentObject));

    if (plan.removesOverlappingAnnotations()) {
        stats.removedAnnotations += removeOverlappingAnnotations(document, pageObject, job.areas);
    }

    stats.removedStrings += edited.stats.removedStrings;
    stats.removedImages += edited.stats.removedImages;
    stats.removedInlineImages += edited.stats.removedInlineImages;
    stats.editedForms += edited.stats.editedForms;
    ++stats.pagesTouched;
    return true;
}

ApplyResult applyPlan(std::string sourceBytes, const domain::RedactionPlan& plan) {
    ApplyResult result;
    PdfDocumentRewriter document;
    std::string openDiagnostic;
    const objects::SourceStatus status = document.open(std::move(sourceBytes), &openDiagnostic);
    if (status != objects::SourceStatus::Ok) {
        result.diagnostic = std::string{objects::describe(status)};
        if (status == objects::SourceStatus::Encrypted) {
            result.diagnostic =
                "加密文件不支援塗黑：字串與串流需要加密後才寫得回去，"
                "半套的實作會產出看起來成功但讀不出來的檔案";
        }
        if (!openDiagnostic.empty()) result.diagnostic += "：" + openDiagnostic;
        return result;
    }

    std::map<int, PageJob> jobs;
    for (const domain::RedactionMark& mark : plan.marks().marks()) {
        if (!mark.isValid()) {
            result.diagnostic = "塗黑標記無效：頁碼為負或區域為空";
            return result;
        }
        PageJob& job = jobs[mark.pageIndex];
        for (const domain::RectF& area : mark.areas) job.areas.push_back(area.normalized());
        job.marks.push_back(mark);
    }

    for (const auto& [pageIndex, job] : jobs) {
        if (!applyToPage(document, pageIndex, job, plan, result.stats, result.diagnostic)) {
            return result;
        }
    }

    result.bytes = document.build();
    result.ok = true;
    return result;
}

}  // namespace

ApplyResult applyRedactions(std::string sourceBytes, const domain::RedactionPlan& plan) {
    return applyPlan(std::move(sourceBytes), plan);
}

ApplyResult applyMarkedRedactions(std::string sourceBytes, domain::IrreversibleConsent consent) {
    ApplyResult result;
    objects::PdfSourceDocument source;
    std::string diagnostic;
    const objects::SourceStatus status = source.open(sourceBytes, &diagnostic);
    if (status != objects::SourceStatus::Ok) {
        result.diagnostic = std::string{objects::describe(status)};
        if (!diagnostic.empty()) result.diagnostic += "：" + diagnostic;
        return result;
    }

    const domain::RedactionMarkSet marks = readRedactionMarks(source);
    if (marks.empty()) {
        result.diagnostic = "文件中沒有任何 /Redact 標記";
        return result;
    }
    return applyPlan(std::move(sourceBytes), domain::RedactionPlan{marks, consent});
}

}  // namespace alioth::engine::redaction
