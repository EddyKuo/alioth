#include "engine/pageops/page_merge.h"

#include <algorithm>
#include <set>

#include "engine/pageops/annotation_transform.h"
#include "engine/pageops/object_copier.h"
#include "engine/pageops/page_form.h"

namespace alioth::engine::pageops {
namespace {

using domain::RectF;
using domain::compose::Matrix;

}  // namespace

MergePagesResult mergePages(std::string sourceBytes, const MergePagesRequest& request) {
    MergePagesResult result;

    ComposeDocument document;
    std::string diagnostic;
    const PageOpsStatus opened = document.open(std::move(sourceBytes), &diagnostic);
    if (opened != PageOpsStatus::Ok) {
        result.status = opened;
        result.diagnostic = diagnostic;
        return result;
    }

    if (request.pages.empty()) {
        result.status = PageOpsStatus::InvalidRequest;
        result.diagnostic = "沒有指定要合併的頁面";
        return result;
    }
    std::set<int> unique;
    for (const int index : request.pages) {
        if (index < 0 || index >= document.pageCount()) {
            result.status = PageOpsStatus::PageOutOfRange;
            result.diagnostic = "頁碼 " + std::to_string(index) + " 超出範圍";
            return result;
        }
        if (!unique.insert(index).second) {
            result.status = PageOpsStatus::InvalidRequest;
            result.diagnostic = "同一頁不可重複合併";
            return result;
        }
    }

    // 先做素材再算版面：格子尺寸取決於來源頁**套用 /Rotate 之後**的尺寸，
    // 而那個尺寸只有在包成 Form XObject 時才確定下來。
    ObjectCopier copier(document.rewriter(), document.rewriter());
    std::vector<PageFormResult> forms;
    std::vector<RectF> boxes;
    forms.reserve(request.pages.size());
    boxes.reserve(request.pages.size());
    for (const int index : request.pages) {
        const PageFormResult form = makePageFormXObject(document, index, document, copier);
        if (!form.ok) {
            result.status = PageOpsStatus::ContentUnreadable;
            result.diagnostic = form.diagnostic;
            return result;
        }
        boxes.push_back(RectF{0.0, 0.0, form.size.width, form.size.height});
        forms.push_back(form);
    }

    const domain::compose::MergePlan plan = domain::compose::planMerge(request.layout, boxes);
    if (!plan.ok()) {
        result.status = PageOpsStatus::LayoutRejected;
        result.compose = plan.status;
        result.diagnostic = domain::compose::describe(plan.status);
        return result;
    }

    // 合併頁先建成空殼，因為註解的 /P 需要它的物件編號。
    PdfDictionary merged;
    merged.set("Type", objects::makeName("Page"));
    merged.set("MediaBox", makeRectArray(RectF{0.0, 0.0, plan.pageSize.width, plan.pageSize.height}));
    const PdfRef mergedPage{document.rewriter().addObject(PdfObject{merged}), 0};

    PdfDictionary xobjects;
    std::string content;
    std::vector<int> annotations;
    std::set<int> visited;

    for (std::size_t i = 0; i < plan.placements.size(); ++i) {
        const domain::compose::Placement& placement = plan.placements[i];
        const PageFormResult& form = forms[static_cast<std::size_t>(placement.sourceIndex)];

        // 最終矩陣 = 來源頁座標 → 素材空間（含 /Rotate 與原點正規化）→ 格子。
        // 內容與註解**必須**共用這一個矩陣，分開算就是兩份會各自漂移的推導。
        const Matrix matrix = domain::compose::concat(form.baseMatrix, placement.matrix);

        const std::string name = "Fx" + std::to_string(i);
        xobjects.set(name, objects::makeRef(form.objectNumber));

        // 外層 q/Q：Form XObject 已經是規格層級的隔離邊界，這一層是為了
        // 讓「某家檢視器沒完全照做」的情況也不會讓 cm 外溢到下一格。
        content += "q\n";
        content += formatMatrix(matrix);
        content += " cm\n/";
        content += name;
        content += " Do\nQ\n";

        if (!request.moveAnnotations) continue;

        PdfRef sourcePage{};
        if (!document.pageAt(request.pages[static_cast<std::size_t>(placement.sourceIndex)],
                             sourcePage)) {
            continue;
        }
        for (const int annotation : pageAnnotationObjects(document.rewriter(), sourcePage)) {
            if (visited.count(annotation) != 0) continue;
            if (transformAnnotation(document.rewriter(), annotation, matrix, &mergedPage, visited)) {
                annotations.push_back(annotation);
                ++result.movedAnnotations;
            }
        }
        // 來源頁不再持有這些註解：留著的話，保留來源頁的模式下同一個物件
        // 會同時掛在兩頁上，而它的幾何已經被改成合併頁的座標。
        setPageAnnotationObjects(document.rewriter(), sourcePage, {});
    }

    PdfDictionary resources;
    resources.set("XObject", PdfObject{std::move(xobjects)});

    const int contentObject = addContentStream(document.rewriter(), std::move(content));
    if (PdfObject* page = document.rewriter().object(mergedPage.number)) {
        if (PdfDictionary* dict = page->asDictionary()) {
            dict->set("Resources", PdfObject{std::move(resources)});
            dict->set("Contents", objects::makeRef(contentObject));
            if (!annotations.empty()) {
                PdfArray array;
                array.reserve(annotations.size());
                for (const int number : annotations) array.push_back(objects::makeRef(number));
                dict->set("Annots", PdfObject{std::move(array)});
            }
        }
    }

    // 頁序：先算出插入位置（以「移除來源頁之後」的座標算），再插入。
    const int firstSource = *std::min_element(request.pages.begin(), request.pages.end());
    std::vector<PdfRef> pages;
    int insertAt = 0;
    if (request.removeSourcePages) {
        for (int i = 0; i < document.pageCount(); ++i) {
            if (unique.count(i) != 0) continue;
            if (i < firstSource) ++insertAt;
            pages.push_back(document.pages()[static_cast<std::size_t>(i)]);
        }
    } else {
        pages = document.pages();
        insertAt = request.insertAt >= 0 ? request.insertAt : firstSource;
    }
    if (request.insertAt >= 0 && request.removeSourcePages) insertAt = request.insertAt;
    insertAt = std::clamp(insertAt, 0, static_cast<int>(pages.size()));
    pages.insert(pages.begin() + insertAt, mergedPage);
    document.setPages(std::move(pages));

    result.mergedPageIndex = insertAt;
    result.pageCount = document.pageCount();
    result.bytes = document.build();
    return result;
}

}  // namespace alioth::engine::pageops
