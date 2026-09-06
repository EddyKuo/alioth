#include "engine/bookmarks/outline_reader.h"

#include <set>
#include <variant>
#include <vector>

#include "engine/bookmarks/destination_codec.h"

namespace alioth::engine::bookmarks {

using domain::bookmarks::Bookmark;
using domain::bookmarks::BookmarkTree;
using domain::bookmarks::TargetEncoding;
using domain::bookmarks::TargetKind;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfSourceDocument;
using objects::PdfString;

namespace {

constexpr int kMaxOutlineDepth = 32;
// 單一文件的書籤上限。沒有上限的話，一個 /Next 指回自己的檔案就能讓記憶體
// 一路長到 OOM；已訪問集合擋得住簡單的迴圈，擋不住每次都生成新編號的情況。
constexpr std::size_t kMaxItems = 200000;

// 以「索引路徑」而不是指標記住要插到哪一層。
//
// 用 BookmarkTree* 是很自然的寫法，但它是錯的：兄弟節點會持續 push_back 到
// 同一個 vector，一旦重新配置，堆疊裡先前存下的子容器指標就全部懸空。
// 症狀不是崩潰而是隨機少掉幾個子節點——書籤數量少時永遠測不出來。
struct Frame {
    int object;
    domain::bookmarks::BookmarkPath parentPath;
    int depth;
};

}  // namespace

OutlineReadResult readOutline(const PdfSourceDocument& source) {
    OutlineReadResult result;

    const int catalogNumber = catalogObjectNumber(source);
    if (catalogNumber <= 0) {
        result.diagnostic = "找不到 catalog（trailer 缺少 /Root）";
        return result;
    }
    const PdfObject catalog = source.object(catalogNumber);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) {
        result.diagnostic = "catalog 不是字典";
        return result;
    }

    // 沒有 /Outlines 是完全合法的（多數文件沒有書籤），因此是成功而不是錯誤。
    const PdfObject* outlinesEntry = catalogDict->find("Outlines");
    result.ok = true;
    if (outlinesEntry == nullptr) return result;

    const PdfObject outlines = source.resolve(*outlinesEntry);
    const PdfDictionary* outlinesDict = outlines.asDictionary();
    if (outlinesDict == nullptr) return result;

    const PdfObject* firstEntry = outlinesDict->find("First");
    if (firstEntry == nullptr || !firstEntry->isRef()) return result;

    const PageIndexMap pages(source);
    std::set<int> visited;

    // 迭代而非遞迴（SDD §7：不可信任輸入決定的深度一律改成迭代）。
    std::vector<Frame> stack;
    stack.push_back(Frame{firstEntry->asRef().number, domain::bookmarks::BookmarkPath{}, 0});

    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (domain::bookmarks::containerAt(result.tree, frame.parentPath) == nullptr) continue;

        int current = frame.object;
        while (current > 0) {
            if (!visited.insert(current).second) {
                ++result.truncated;
                break;
            }
            if (result.itemCount >= kMaxItems) {
                ++result.truncated;
                break;
            }

            const PdfObject item = source.object(current);
            const PdfDictionary* dict = item.asDictionary();
            if (dict == nullptr) break;

            Bookmark node;
            if (const PdfObject* title = dict->find("Title")) {
                const PdfObject resolvedTitle = source.resolve(*title);
                if (const PdfString* string = std::get_if<PdfString>(&resolvedTitle.value())) {
                    node.title = decodeTextString(*string);
                }
            }

            // /Dest 優先於 /A：兩者同時存在時 ISO 32000-2 §12.3.3 規定以 /Dest 為準。
            domain::bookmarks::Destination destination;
            std::string name;
            bool decoded = false;
            TargetEncoding encoding = TargetEncoding::Dest;
            if (const PdfObject* dest = dict->find("Dest")) {
                decoded = decodeDestination(source, pages, *dest, destination, name);
            } else if (const PdfObject* action = dict->find("A")) {
                encoding = TargetEncoding::GoToAction;
                const PdfObject resolvedAction = source.resolve(*action);
                if (const PdfDictionary* actionDict = resolvedAction.asDictionary()) {
                    const PdfObject* type = actionDict->find("S");
                    // 只認 GoTo。/URI 與 /Launch 是另一回事，後者一律禁止
                    // （CLAUDE.md「PDF 視為不可信任輸入」），這裡不把它當成跳轉目標。
                    if (type != nullptr && source.resolve(*type).isName("GoTo")) {
                        if (const PdfObject* inner = actionDict->find("D")) {
                            decoded = decodeDestination(source, pages, *inner, destination, name);
                        }
                    }
                }
            }
            if (decoded) {
                node.target = domain::bookmarks::BookmarkTarget::direct(destination, encoding);
            } else if (!name.empty()) {
                node.target = domain::bookmarks::BookmarkTarget::named(name, encoding);
            }

            if (const PdfObject* color = dict->find("C")) {
                const PdfObject resolvedColor = source.resolve(*color);
                if (const PdfArray* array = resolvedColor.asArray()) {
                    if (array->size() >= 3) {
                        node.hasColor = true;
                        node.colorR = (*array)[0].asNumber();
                        node.colorG = (*array)[1].asNumber();
                        node.colorB = (*array)[2].asNumber();
                    }
                }
            }
            if (const PdfObject* flags = dict->find("F")) {
                const std::int64_t value = source.resolve(*flags).asInteger();
                node.italic = (value & 1) != 0;
                node.bold = (value & 2) != 0;
            }
            if (const PdfObject* count = dict->find("Count")) {
                node.open = source.resolve(*count).asInteger() > 0;
            }

            BookmarkTree* container = domain::bookmarks::containerAt(result.tree, frame.parentPath);
            container->push_back(std::move(node));
            domain::bookmarks::BookmarkPath myPath = frame.parentPath;
            myPath.push_back(container->size() - 1);
            ++result.itemCount;

            if (frame.depth + 1 < kMaxOutlineDepth) {
                if (const PdfObject* first = dict->find("First")) {
                    if (first->isRef() && first->asRef().number > 0) {
                        stack.push_back(
                            Frame{first->asRef().number, std::move(myPath), frame.depth + 1});
                    }
                }
            } else if (dict->has("First")) {
                ++result.truncated;
            }

            const PdfObject* next = dict->find("Next");
            current = (next != nullptr && next->isRef()) ? next->asRef().number : 0;
        }
    }

    return result;
}

}  // namespace alioth::engine::bookmarks
