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

// 一組來源頁合成出來的結果。失敗時只帶診斷，呼叫端決定要不要中止整份文件。
struct ComposedGroup {
    bool ok{false};
    PageOpsStatus status{PageOpsStatus::Ok};
    domain::compose::ComposeStatus compose{domain::compose::ComposeStatus::Ok};
    std::string diagnostic;
    PdfRef page{};
    int movedAnnotations{0};
};

// 把一組來源頁合成成一頁，寫進已開啟的文件並回傳新頁的參照。
//
// 抽出來共用是為了 mergePageGroups：一次要合成很多組，而每一組的步驟
// （包 Form XObject → 算版面 → 寫內容串流 → 搬註解）完全相同。逐組各開一次
// 文件也做得到，但那是每組一次全檔重寫——100 組就是重寫 100 次一份可能
// 100 MB 的檔案，而症狀只是「很慢」，不會有任何人看得出原因。
//
// visitedAnnotations 跨組共用：同一個註解物件被搬兩次的話，第二次的矩陣會
// 疊在第一次的結果上，標記會飛到頁面外。
[[nodiscard]] ComposedGroup composeGroup(ComposeDocument& document, ObjectCopier& copier,
                                         const std::vector<int>& pages,
                                         const domain::compose::MergeLayout& layout,
                                         bool moveAnnotations,
                                         std::set<int>& visitedAnnotations) {
    ComposedGroup result;

    // 先做素材再算版面：格子尺寸取決於來源頁**套用 /Rotate 之後**的尺寸，
    // 而那個尺寸只有在包成 Form XObject 時才確定下來。
    std::vector<PageFormResult> forms;
    std::vector<RectF> boxes;
    forms.reserve(pages.size());
    boxes.reserve(pages.size());
    for (const int index : pages) {
        const PageFormResult form = makePageFormXObject(document, index, document, copier);
        if (!form.ok) {
            result.status = PageOpsStatus::ContentUnreadable;
            result.diagnostic = form.diagnostic;
            return result;
        }
        boxes.push_back(RectF{0.0, 0.0, form.size.width, form.size.height});
        forms.push_back(form);
    }

    const domain::compose::MergePlan plan = domain::compose::planMerge(layout, boxes);
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

        if (!moveAnnotations) continue;

        PdfRef sourcePage{};
        if (!document.pageAt(pages[static_cast<std::size_t>(placement.sourceIndex)], sourcePage)) {
            continue;
        }
        for (const int annotation : pageAnnotationObjects(document.rewriter(), sourcePage)) {
            if (visitedAnnotations.count(annotation) != 0) continue;
            if (transformAnnotation(document.rewriter(), annotation, matrix, &mergedPage,
                                    visitedAnnotations)) {
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

    result.ok = true;
    result.page = mergedPage;
    return result;
}

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

    ObjectCopier copier(document.rewriter(), document.rewriter());
    std::set<int> visited;
    const ComposedGroup group = composeGroup(document, copier, request.pages, request.layout,
                                             request.moveAnnotations, visited);
    if (!group.ok) {
        result.status = group.status;
        result.compose = group.compose;
        result.diagnostic = group.diagnostic;
        return result;
    }
    result.movedAnnotations = group.movedAnnotations;

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
    pages.insert(pages.begin() + insertAt, group.page);
    document.setPages(std::move(pages));

    result.mergedPageIndex = insertAt;
    result.pageCount = document.pageCount();
    result.bytes = document.build();
    return result;
}

MergeGroupsResult mergePageGroups(std::string sourceBytes, const MergeGroupsRequest& request) {
    MergeGroupsResult result;

    ComposeDocument document;
    std::string diagnostic;
    const PageOpsStatus opened = document.open(std::move(sourceBytes), &diagnostic);
    if (opened != PageOpsStatus::Ok) {
        result.status = opened;
        result.diagnostic = diagnostic;
        return result;
    }

    if (request.groups.empty()) {
        result.status = PageOpsStatus::InvalidRequest;
        result.diagnostic = "沒有指定要合併的頁面組";
        return result;
    }

    // 全部組別的來源頁必須互斥：同一頁進兩組的話，第二組拿到的是一個已經
    // 被搬空註解、而且即將從頁面樹移除的頁面——輸出不會錯得很明顯，
    // 只是那一頁的標記整組消失。
    std::set<int> claimed;
    for (const MergeGroup& group : request.groups) {
        if (group.pages.empty()) {
            result.status = PageOpsStatus::InvalidRequest;
            result.diagnostic = "有一組沒有指定任何頁面";
            return result;
        }
        for (const int index : group.pages) {
            if (index < 0 || index >= document.pageCount()) {
                result.status = PageOpsStatus::PageOutOfRange;
                result.diagnostic = "頁碼 " + std::to_string(index) + " 超出範圍";
                return result;
            }
            if (!claimed.insert(index).second) {
                result.status = PageOpsStatus::InvalidRequest;
                result.diagnostic = "頁碼 " + std::to_string(index) + " 被多組重複使用";
                return result;
            }
        }
    }

    ObjectCopier copier(document.rewriter(), document.rewriter());
    std::set<int> visited;
    // 每一組合成頁，連同它「該站在原本哪一頁的位置」。
    std::vector<std::pair<int, PdfRef>> composed;  // (組內最小的原始頁碼, 新頁)
    composed.reserve(request.groups.size());
    for (const MergeGroup& group : request.groups) {
        const ComposedGroup made = composeGroup(document, copier, group.pages, group.layout,
                                                request.moveAnnotations, visited);
        if (!made.ok) {
            result.status = made.status;
            result.compose = made.compose;
            result.diagnostic = made.diagnostic;
            return result;
        }
        result.movedAnnotations += made.movedAnnotations;
        composed.emplace_back(*std::min_element(group.pages.begin(), group.pages.end()), made.page);
    }

    std::sort(composed.begin(), composed.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    // 頁序：走一遍原始頁碼，遇到某一組的第一頁就放它的合成頁，組內其餘頁跳過。
    // 這樣每個合成頁都留在它來源頁原本的位置，不需要任何「由後往前插入」的技巧
    // ——那個技巧一旦寫反，症狀是「第 30 頁之後全部對不上」，兩頁的測試文件
    // 完全看不出來。
    std::vector<PdfRef> pages;
    pages.reserve(document.pages().size());
    std::size_t next = 0;
    for (int i = 0; i < document.pageCount(); ++i) {
        if (next < composed.size() && composed[next].first == i) {
            pages.push_back(composed[next].second);
            result.mergedPageIndices.push_back(static_cast<int>(pages.size()) - 1);
            ++next;
            if (request.removeSourcePages) continue;
        }
        if (request.removeSourcePages && claimed.count(i) != 0) continue;
        pages.push_back(document.pages()[static_cast<std::size_t>(i)]);
    }
    document.setPages(std::move(pages));

    result.pageCount = document.pageCount();
    result.bytes = document.build();
    return result;
}

}  // namespace alioth::engine::pageops
