#include "engine/pageops/page_overlay.h"

#include <algorithm>
#include <map>
#include <set>

#include "engine/pageops/annotation_transform.h"
#include "engine/pageops/object_copier.h"
#include "engine/pageops/page_form.h"

namespace alioth::engine::pageops {
namespace {

using domain::RectF;
using domain::compose::Matrix;

// 取代頁面時要跟著搬的鍵。
//
// 這裡是白名單而不是黑名單，理由是安全性與體積：/Parent 指回來源文件的頁面樹、
// /StructParents 牽到整棵結構樹、/PieceInfo 牽到產生軟體的私有資料，
// 順著複製會把整份來源文件搬進主文件，而且多半沒人會發現——只會覺得檔案很大。
constexpr const char* kCopiedPageKeys[] = {
    "Contents", "Resources", "MediaBox", "CropBox", "BleedBox", "TrimBox",
    "ArtBox",   "Rotate",    "Annots",   "Group",   "UserUnit",
};

// 在頁面既有的 /Resources /XObject 裡找一個不會撞名的名稱。
// 撞名不會報錯，只會讓原本的影像被換成我們的浮水印。
std::string uniqueXObjectName(const PdfDocumentRewriter& document, const PdfRef& page) {
    std::set<std::string> used;
    if (const PdfDictionary* pageDict = pageDictionary(document, page)) {
        if (const PdfObject* resources = pageDict->find("Resources")) {
            const PdfObject resolved = document.resolve(*resources);
            if (const PdfDictionary* dict = resolved.asDictionary()) {
                if (const PdfObject* group = dict->find("XObject")) {
                    const PdfObject groupResolved = document.resolve(*group);
                    if (const PdfDictionary* names = groupResolved.asDictionary()) {
                        for (const auto& entry : names->entries()) used.insert(entry.first);
                    }
                }
            }
        }
    }
    for (int i = 0;; ++i) {
        std::string candidate = "AliothOv" + std::to_string(i);
        if (used.count(candidate) == 0) return candidate;
    }
}

std::vector<int> resolvePageSelection(const std::vector<int>& requested, int pageCount, bool& ok) {
    ok = true;
    std::vector<int> pages;
    if (requested.empty()) {
        pages.reserve(static_cast<std::size_t>(pageCount));
        for (int i = 0; i < pageCount; ++i) pages.push_back(i);
        return pages;
    }
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

OverlayResult overlayDocument(std::string baseBytes, std::string overlayBytes,
                              const OverlayRequest& request) {
    OverlayResult result;

    ComposeDocument base;
    std::string diagnostic;
    if (base.open(std::move(baseBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SourceInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    ComposeDocument overlay;
    diagnostic.clear();
    if (overlay.open(std::move(overlayBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SecondaryInvalid;
        result.diagnostic = diagnostic;
        return result;
    }
    if (overlay.pageCount() <= 0) {
        result.status = PageOpsStatus::SecondaryInvalid;
        result.diagnostic = "覆蓋用的文件沒有頁面";
        return result;
    }

    bool selectionOk = false;
    const std::vector<int> targets =
        resolvePageSelection(request.basePages, base.pageCount(), selectionOk);
    if (!selectionOk) {
        result.status = PageOpsStatus::PageOutOfRange;
        result.diagnostic = "底頁頁碼超出範圍";
        return result;
    }
    if (request.overlayPageIndex >= overlay.pageCount()) {
        result.status = PageOpsStatus::PageOutOfRange;
        result.diagnostic = "覆蓋頁頁碼超出範圍";
        return result;
    }

    ObjectCopier copier(overlay.rewriter(), base.rewriter());
    // 同一個來源頁可能疊到很多底頁，Form XObject 只做一次：每頁各做一份的話，
    // 一份 500 頁的浮水印文件會膨脹 500 倍。
    std::map<int, PageFormResult> forms;

    for (std::size_t i = 0; i < targets.size(); ++i) {
        const int baseIndex = targets[i];
        PdfRef basePage{};
        if (!base.pageAt(baseIndex, basePage)) continue;

        const int overlayIndex =
            request.overlayPageIndex >= 0
                ? request.overlayPageIndex
                : static_cast<int>(i % static_cast<std::size_t>(overlay.pageCount()));

        auto found = forms.find(overlayIndex);
        if (found == forms.end()) {
            const PageFormResult form = makePageFormXObject(overlay, overlayIndex, base, copier);
            if (!form.ok) {
                result.status = PageOpsStatus::ContentUnreadable;
                result.diagnostic = form.diagnostic;
                return result;
            }
            found = forms.emplace(overlayIndex, form).first;
        }
        const PageFormResult& form = found->second;

        const domain::compose::OverlayPlacement placement = domain::compose::planOverlay(
            request.options, base.cropBox(basePage),
            RectF{0.0, 0.0, form.size.width, form.size.height});
        if (!placement.ok()) {
            result.status = PageOpsStatus::LayoutRejected;
            result.compose = placement.status;
            result.diagnostic = domain::compose::describe(placement.status);
            return result;
        }
        const Matrix matrix = domain::compose::concat(form.baseMatrix, placement.matrix);

        const std::string name = uniqueXObjectName(base.rewriter(), basePage);
        if (!redaction::setResourceEntry(base.rewriter(), basePage.number, "XObject", name,
                                         objects::makeRef(form.objectNumber))) {
            result.status = PageOpsStatus::InvalidRequest;
            result.diagnostic = "無法在底頁登記 XObject 資源";
            return result;
        }

        std::string content = "q\n";
        content += formatMatrix(matrix);
        content += " cm\n/";
        content += name;
        content += " Do\nQ\n";
        const int contentObject = addContentStream(base.rewriter(), std::move(content));

        std::vector<int> contents = wrapPageContentInGraphicsState(base.rewriter(), basePage);
        if (request.options.layer == domain::compose::OverlayLayer::Above) {
            contents.push_back(contentObject);
        } else {
            contents.insert(contents.begin(), contentObject);
        }
        setPageContents(base.rewriter(), basePage, contents);
        ++result.overlaidPages;

        if (!request.copyAnnotations) continue;

        PdfRef overlayPage{};
        if (!overlay.pageAt(overlayIndex, overlayPage)) continue;
        std::vector<int> annotations = pageAnnotationObjects(base.rewriter(), basePage);
        std::set<int> visited;
        for (const int sourceAnnotation : pageAnnotationObjects(overlay.rewriter(), overlayPage)) {
            const int copied = copier.copyObject(sourceAnnotation);
            if (copied <= 0) continue;
            const PdfObject* template_ = base.rewriter().object(copied);
            if (template_ == nullptr) continue;
            // 每個底頁都要一份獨立的註解物件複本：copier 的快取會讓第二個底頁
            // 拿到同一個編號，而它的幾何已經被第一個底頁的矩陣改過了。
            const int owned = base.rewriter().addObject(*template_);
            if (!transformAnnotation(base.rewriter(), owned, matrix, &basePage, visited)) continue;
            annotations.push_back(owned);
            ++result.copiedAnnotations;
        }
        setPageAnnotationObjects(base.rewriter(), basePage, annotations);
    }

    result.pageCount = base.pageCount();
    result.bytes = base.build();
    return result;
}

// 把來源文件的一頁複製進主文件，回傳新頁面物件。複製的鍵集合由 kCopiedPageKeys
// 決定；註解的 /P 一併改指到新頁面，否則它仍指向來源文件裡那一頁的複本
// ——多數檢視器不看 /P，但表單與回覆串（/IRT）看。
[[nodiscard]] bool copyPageInto(ComposeDocument& source, int sourceIndex, ComposeDocument& base,
                                ObjectCopier& copier, PdfRef& out) {
    PdfRef sourcePage{};
    if (!source.pageAt(sourceIndex, sourcePage)) return false;
    const PdfDictionary* sourceDict = pageDictionary(source.rewriter(), sourcePage);
    if (sourceDict == nullptr) return false;

    PdfDictionary copied;
    copied.set("Type", objects::makeName("Page"));
    for (const char* key : kCopiedPageKeys) {
        if (const PdfObject* value = sourceDict->find(key)) {
            copied.set(key, copier.copyValue(*value));
        }
    }
    out = PdfRef{base.rewriter().addObject(PdfObject{std::move(copied)}), 0};

    std::set<int> visited;
    for (const int annotation : pageAnnotationObjects(base.rewriter(), out)) {
        (void)transformAnnotation(base.rewriter(), annotation, domain::compose::identity(), &out,
                                  visited);
    }
    return true;
}

ReplaceResult replacePages(std::string baseBytes, std::string replacementBytes,
                           const ReplaceRequest& request) {
    ReplaceResult result;

    ComposeDocument base;
    std::string diagnostic;
    if (base.open(std::move(baseBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SourceInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    ComposeDocument replacement;
    diagnostic.clear();
    if (replacement.open(std::move(replacementBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SecondaryInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    if (request.pageCount < 0 || request.firstPage < 0 ||
        request.firstPage + request.pageCount > base.pageCount()) {
        result.status = PageOpsStatus::PageOutOfRange;
        result.diagnostic = "取代範圍超出主文件頁數";
        return result;
    }

    bool selectionOk = false;
    const std::vector<int> sources =
        resolvePageSelection(request.replacementPages, replacement.pageCount(), selectionOk);
    if (!selectionOk) {
        result.status = PageOpsStatus::PageOutOfRange;
        result.diagnostic = "取代來源頁碼超出範圍";
        return result;
    }
    if (sources.empty()) {
        result.status = PageOpsStatus::InvalidRequest;
        result.diagnostic = "沒有可用來取代的頁面";
        return result;
    }

    ObjectCopier copier(replacement.rewriter(), base.rewriter());
    std::vector<PdfRef> inserted;
    inserted.reserve(sources.size());

    for (const int index : sources) {
        PdfRef newPage{};
        if (!copyPageInto(replacement, index, base, copier, newPage)) continue;
        inserted.push_back(newPage);
    }

    std::vector<PdfRef> pages = base.pages();
    const auto begin = pages.begin() + request.firstPage;
    pages.erase(begin, begin + request.pageCount);
    pages.insert(pages.begin() + request.firstPage, inserted.begin(), inserted.end());
    base.setPages(std::move(pages));

    result.removedPages = request.pageCount;
    result.insertedPages = static_cast<int>(inserted.size());
    result.pageCount = base.pageCount();
    result.bytes = base.build();
    return result;
}

ReplaceResult interleavePagesFrom(std::string baseBytes, std::string sourceBytes,
                                  const std::vector<PagePlacement>& placements) {
    ReplaceResult result;

    ComposeDocument base;
    std::string diagnostic;
    if (base.open(std::move(baseBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SourceInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    ComposeDocument source;
    diagnostic.clear();
    if (source.open(std::move(sourceBytes), &diagnostic) != PageOpsStatus::Ok) {
        result.status = PageOpsStatus::SecondaryInvalid;
        result.diagnostic = diagnostic;
        return result;
    }

    if (placements.empty()) {
        result.status = PageOpsStatus::InvalidRequest;
        result.diagnostic = "沒有指定任何插入點";
        return result;
    }
    for (const PagePlacement& placement : placements) {
        if (placement.sourcePage < 0 || placement.sourcePage >= source.pageCount()) {
            result.status = PageOpsStatus::PageOutOfRange;
            result.diagnostic = "插入來源頁碼超出範圍";
            return result;
        }
        // -1 代表插在第一頁之前；其餘必須是主文件裡真實存在的頁。
        if (placement.afterBasePage < -1 || placement.afterBasePage >= base.pageCount()) {
            result.status = PageOpsStatus::PageOutOfRange;
            result.diagnostic = "插入位置超出主文件頁數";
            return result;
        }
    }

    ObjectCopier copier(source.rewriter(), base.rewriter());
    std::vector<PdfRef> copied;
    copied.reserve(placements.size());
    for (const PagePlacement& placement : placements) {
        PdfRef newPage{};
        if (!copyPageInto(source, placement.sourcePage, base, copier, newPage)) {
            result.status = PageOpsStatus::SecondaryInvalid;
            result.diagnostic = "來源頁面無法複製";
            return result;
        }
        copied.push_back(newPage);
    }

    // 一次算出最終順序，而不是逐一插入。
    //
    // 逐一插入的話每插一頁後面的索引就位移一格，呼叫端必須自己由後往前排；
    // 那個要求沒有任何地方擋得住，一旦有人由前往後呼叫，摘要頁會愈插愈偏，
    // 而症狀是「第 30 頁之後的摘要對不上」——不會有人在兩頁的測試文件上發現。
    const std::vector<PdfRef>& basePages = base.pages();
    std::vector<PdfRef> ordered;
    ordered.reserve(basePages.size() + copied.size());
    const auto appendFor = [&](int afterIndex) {
        for (std::size_t i = 0; i < placements.size(); ++i) {
            if (placements[i].afterBasePage == afterIndex) ordered.push_back(copied[i]);
        }
    };
    appendFor(-1);
    for (int i = 0; i < static_cast<int>(basePages.size()); ++i) {
        ordered.push_back(basePages[static_cast<std::size_t>(i)]);
        appendFor(i);
    }

    result.insertedPages = static_cast<int>(copied.size());
    base.setPages(std::move(ordered));
    result.pageCount = base.pageCount();
    result.bytes = base.build();
    result.status = PageOpsStatus::Ok;
    return result;
}

ReplaceResult insertPagesFrom(std::string baseBytes, std::string sourceBytes, int atIndex,
                              std::vector<int> sourcePages) {
    ReplaceRequest request;
    request.firstPage = atIndex;
    // 零頁被取代 = 純插入。這一行就是本函式存在的全部理由——把它藏在
    // 呼叫端會讓某天有人寫成 1，於是插入靜默地吃掉插入點那一頁。
    request.pageCount = 0;
    request.replacementPages = std::move(sourcePages);
    return replacePages(std::move(baseBytes), std::move(sourceBytes), request);
}

}  // namespace alioth::engine::pageops
