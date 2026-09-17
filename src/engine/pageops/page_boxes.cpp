#include "engine/pageops/page_boxes.h"

#include <set>

#include "engine/pageops/annotation_transform.h"

namespace alioth::engine::pageops {
namespace {

using domain::RectF;
using domain::compose::Matrix;
using domain::compose::PageBoxKind;

std::vector<int> selectPages(const std::vector<int>& requested, int pageCount, bool& ok) {
    ok = true;
    if (requested.empty()) {
        std::vector<int> all;
        all.reserve(static_cast<std::size_t>(pageCount));
        for (int i = 0; i < pageCount; ++i) all.push_back(i);
        return all;
    }
    std::vector<int> pages;
    for (const int index : requested) {
        if (index < 0 || index >= pageCount) {
            ok = false;
            return {};
        }
        pages.push_back(index);
    }
    return pages;
}

}  // namespace

PageBoxResult setPageBoxes(std::string sourceBytes, const PageBoxRequest& request) {
    PageBoxResult result;

    ComposeDocument document;
    std::string diagnostic;
    if (document.open(std::move(sourceBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SourceInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    if (request.settings.empty()) {
        result.status = PageOpsStatus::InvalidRequest;
        result.compose = domain::compose::ComposeStatus::NothingToDo;
        result.diagnostic = "沒有指定任何要設定的框";
        return result;
    }

    bool ok = false;
    const std::vector<int> pages = selectPages(request.pages, document.pageCount(), ok);
    if (!ok) {
        result.status = PageOpsStatus::PageOutOfRange;
        result.diagnostic = "頁碼超出範圍";
        return result;
    }

    for (const int index : pages) {
        PdfRef page{};
        if (!document.pageAt(index, page)) continue;

        const domain::compose::ResolvedPageBoxes resolved =
            domain::compose::resolvePageBoxes(request.settings, document.mediaBox(page));
        if (!resolved.ok()) {
            result.status = PageOpsStatus::LayoutRejected;
            result.compose = resolved.status;
            result.diagnostic = domain::compose::describe(resolved.status);
            return result;
        }
        result.clamped = result.clamped || resolved.clamped;

        PdfDictionary* dict = pageDictionary(document.rewriter(), page);
        if (dict == nullptr) continue;

        // /MediaBox 一律寫回：即使呼叫端沒有指定，夾子框的計算也是以它為基準，
        // 寫回去可以讓「頁面上看到的框」與「我們據以計算的框」是同一個東西。
        if (request.settings.media) dict->set("MediaBox", makeRectArray(resolved.media));
        for (const PageBoxKind kind : domain::compose::kAllBoxKinds) {
            if (kind == PageBoxKind::Media) continue;
            if (!request.settings.box(kind)) continue;
            const std::optional<RectF> value = resolved.box(kind);
            if (!value) continue;
            dict->set(domain::compose::boxKey(kind), makeRectArray(*value));
        }
        ++result.changedPages;
    }

    result.pageCount = document.pageCount();
    result.bytes = document.build();
    return result;
}

NormalizeResult normalizePages(std::string sourceBytes, const NormalizeRequest& request) {
    NormalizeResult result;

    ComposeDocument document;
    std::string diagnostic;
    if (document.open(std::move(sourceBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SourceInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    bool ok = false;
    const std::vector<int> pages = selectPages(request.pages, document.pageCount(), ok);
    if (!ok) {
        result.status = PageOpsStatus::PageOutOfRange;
        result.diagnostic = "頁碼超出範圍";
        return result;
    }

    for (const int index : pages) {
        PdfRef page{};
        if (!document.pageAt(index, page)) continue;

        const RectF media = document.mediaBox(page);
        const domain::compose::NormalizationPlan plan = domain::compose::planNormalization(media);
        if (!plan.ok()) {
            result.status = PageOpsStatus::LayoutRejected;
            result.compose = plan.status;
            result.diagnostic = domain::compose::describe(plan.status);
            return result;
        }
        // 已經在原點的頁面不重寫：每次開檔存檔都讓檔案長大一段，
        // 使用者會（正確地）認為我們在偷改他的檔案。
        if (!plan.needed) continue;

        const Matrix matrix = plan.matrix();

        // 一、內容。用 q/Q 夾住我們插入的 cm，否則平移量會外溢到後面的內容——
        // 頁面本身沒有問題時看不出來，但頁面用了多個內容串流時會平移兩次。
        std::vector<int> contents = wrapPageContentInGraphicsState(document.rewriter(), page);
        if (!contents.empty()) {
            std::string prefix = "q\n";
            prefix += formatMatrix(matrix);
            prefix += " cm\n";
            contents.insert(contents.begin(), addContentStream(document.rewriter(), prefix));
            contents.push_back(addContentStream(document.rewriter(), "\nQ\n"));
            setPageContents(document.rewriter(), page, contents);
        }

        PdfDictionary* dict = pageDictionary(document.rewriter(), page);
        if (dict == nullptr) continue;

        // 二、五種框。子框沒有跟著平移的話，CropBox 會把頁面裁在錯誤的位置，
        // 而那看起來像是「內容被平移錯了」，會把人引到完全錯誤的方向。
        dict->set("MediaBox", makeRectArray(plan.media));
        for (const PageBoxKind kind : domain::compose::kAllBoxKinds) {
            if (kind == PageBoxKind::Media) continue;
            const char* key = domain::compose::boxKey(kind);
            const PdfObject* value = dict->find(key);
            if (value == nullptr) continue;
            RectF box{};
            if (!readRectArray(document.rewriter(), *value, box)) continue;
            dict->set(key, makeRectArray(domain::compose::translated(box, plan.dx, plan.dy)));
        }

        // 三、註解。這是最容易被漏掉的一項，漏了的症狀是頁面完全正確、
        // 標記整批偏移一個 MediaBox 原點的量。
        if (request.moveAnnotations) {
            std::set<int> visited;
            for (const int annotation : pageAnnotationObjects(document.rewriter(), page)) {
                if (transformAnnotation(document.rewriter(), annotation, matrix, nullptr, visited)) {
                    ++result.movedAnnotations;
                }
            }
        }
        ++result.normalizedPages;
    }

    result.pageCount = document.pageCount();
    result.bytes = document.build();
    return result;
}

std::vector<domain::SizeF> readVisiblePageSizes(const std::string& bytes) {
    std::vector<domain::SizeF> sizes;

    ComposeDocument document;
    if (document.open(bytes) != PageOpsStatus::Ok) return sizes;

    sizes.reserve(static_cast<std::size_t>(document.pageCount()));
    for (const PdfRef& page : document.pages()) {
        const domain::RectF box = document.cropBox(page);
        const int rotation = ((document.rotation(page) % 360) + 360) % 360;
        // 90 與 270 度時長寬互換。這一步漏掉的話，橫向掃描件在並排版面裡會
        // 被算成直的，於是縮到只佔格子一半——畫面上像是「圖變小了」，
        // 而不像是旋轉被忽略。
        const bool quarterTurn = rotation == 90 || rotation == 270;
        sizes.push_back(quarterTurn ? domain::SizeF{box.height(), box.width()}
                                    : domain::SizeF{box.width(), box.height()});
    }
    return sizes;
}

}  // namespace alioth::engine::pageops
