#include "engine/objects/struct_tree_reader.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "domain/text_layer.h"

namespace alioth::engine::objects {
namespace {

// PDF 文字字串 → UTF-8。與 bookmarks/destination_codec.cpp 的實作相同，
// 但不能共用：alioth_bookmarks 連結 alioth_objects，反向相依會成環。
// 這是分層的代價，兩份實作各自有測試涵蓋。
[[nodiscard]] std::string decodeTextString(const PdfString& string) {
    const std::string& bytes = string.bytes;
    std::string out;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            char32_t unit = static_cast<char32_t>((static_cast<unsigned char>(bytes[i]) << 8) |
                                                  static_cast<unsigned char>(bytes[i + 1]));
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
                const char32_t low =
                    static_cast<char32_t>((static_cast<unsigned char>(bytes[i + 2]) << 8) |
                                          static_cast<unsigned char>(bytes[i + 3]));
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            domain::appendUtf8(out, unit);
        }
        return out;
    }
    for (const char c : bytes) domain::appendUtf8(out, static_cast<unsigned char>(c));
    return out;
}

// 取字典裡的文字字串鍵。值可以是間接參照（大段 /Alt 常被放進獨立物件）。
[[nodiscard]] std::string textEntry(const PdfSourceDocument& source, const PdfDictionary& dict,
                                    const char* key) {
    const PdfObject* raw = dict.find(key);
    if (raw == nullptr) return {};
    const PdfObject resolved = source.resolve(*raw);
    if (const auto* string = std::get_if<PdfString>(&resolved.value())) {
        return decodeTextString(*string);
    }
    return {};
}

[[nodiscard]] std::string nameEntry(const PdfSourceDocument& source, const PdfDictionary& dict,
                                    const char* key) {
    const PdfObject* raw = dict.find(key);
    if (raw == nullptr) return {};
    const PdfObject resolved = source.resolve(*raw);
    return resolved.isName() ? resolved.asName() : std::string{};
}

// 頁面物件編號 → 頁序。建一次表比對每個 /Pg 逐頁線性搜尋快得多；
// 十萬個結構元素乘上五百頁的線性搜尋會讓開檔停住好幾秒。
[[nodiscard]] std::unordered_map<int, std::int32_t> buildPageIndexMap(
    const PdfSourceDocument& source) {
    std::unordered_map<int, std::int32_t> map;
    const std::vector<PdfRef>& pages = source.pages();
    map.reserve(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        map.emplace(pages[i].number, static_cast<std::int32_t>(i));
    }
    return map;
}

struct Walker {
    const PdfSourceDocument& source;
    const std::unordered_map<int, std::int32_t>& pageIndex;
    std::unordered_set<int> visiting;  // 迴圈保護：/K 指回祖先的檔案是存在的
    std::size_t nodeCount{0};
    bool truncated{false};

    // 把 /K 的一個項目展開成 0 或 1 個結構元素。
    //
    // /K 的值有五種形態：整數（MCID，內容項而非元素）、參照、字典、陣列，
    // 以及 /Type /MCR 或 /OBJR 的字典。只有 /StructElem 是我們要的節點；
    // 其餘是內容對應，對面板沒有意義，但裸 MCID 會記錄到 owner 身上
    // （見標頭 StructNode::mcids 的說明），不是完全略過。
    //
    // owner 是目前正在組裝、擁有這個 /K 陣列的結構元素；頂層呼叫（/StructTreeRoot
    // 自己的 /K）沒有 owner，傳 nullptr——那個層級不該出現裸 MCID，出現了也只能捨棄。
    void expand(const PdfObject& kid, int depth, std::vector<StructNode>& out, StructNode* owner) {
        if (depth > kMaxStructDepth || nodeCount >= kMaxStructNodes) {
            truncated = true;
            return;
        }

        if (const auto* array = kid.asArray()) {
            for (const PdfObject& item : *array) expand(item, depth, out, owner);
            return;
        }

        const int objectNumber = kid.isRef() ? kid.asRef().number : 0;
        // 已在走訪路徑上代表 /K 形成迴圈。直接返回，不標 truncated——
        // 迴圈是檔案的問題，截斷是我們的上限，兩者的意義不同。
        if (objectNumber != 0 && visiting.count(objectNumber) != 0) return;

        const PdfObject resolved = kid.isRef() ? source.object(objectNumber) : kid;
        const PdfDictionary* dict = resolved.asDictionary();
        if (dict == nullptr) {
            // 裸整數 MCID：屬於 owner 自己的內容，不是子元素。
            if (owner != nullptr) {
                if (const auto* mcid = std::get_if<std::int64_t>(&resolved.value())) {
                    owner->mcids.push_back(static_cast<std::int32_t>(*mcid));
                }
            }
            return;
        }

        const std::string type = nameEntry(source, *dict, "Type");
        // /MCR 是「標記內容參照」的字典形態（ISO 32000-1 §14.7.4.3 表 324），
        // 用在 MCID 與擁有它的結構元素不在同一頁時。/MCID 鍵仍然是這個 owner 的內容，
        // 意義與裸整數相同，只是多包了一層字典，同樣要記錄下來。
        if (type == "MCR") {
            if (owner != nullptr) {
                if (const PdfObject* mcidEntry = dict->find("MCID"); mcidEntry != nullptr) {
                    const PdfObject resolvedMcid = source.resolve(*mcidEntry);
                    if (const auto* mcid = std::get_if<std::int64_t>(&resolvedMcid.value())) {
                        owner->mcids.push_back(static_cast<std::int32_t>(*mcid));
                    }
                }
            }
            return;
        }
        // /OBJR 是「物件參照」（指向頁面內容裡的一個 XObject，例如影像），不帶 MCID，
        // 對閱讀順序沒有直接意義，維持略過。
        if (type == "OBJR") return;
        // 沒有 /S 就不是結構元素。/Type 可省略（ISO 32000-1 表 323），
        // 因此不能靠 /Type == "StructElem" 判斷。
        if (!dict->has("S")) return;

        StructNode node;
        node.type = nameEntry(source, *dict, "S");
        node.title = textEntry(source, *dict, "T");
        node.altText = textEntry(source, *dict, "Alt");
        node.actualText = textEntry(source, *dict, "ActualText");
        node.language = textEntry(source, *dict, "Lang");
        node.expansion = textEntry(source, *dict, "E");
        node.objectNumber = objectNumber;

        if (const PdfObject* pg = dict->find("Pg"); pg != nullptr && pg->isRef()) {
            if (const auto it = pageIndex.find(pg->asRef().number); it != pageIndex.end()) {
                node.pageIndex = it->second;
            }
        }

        ++nodeCount;

        if (const PdfObject* kids = dict->find("K"); kids != nullptr) {
            if (objectNumber != 0) visiting.insert(objectNumber);
            expand(*kids, depth + 1, node.children, &node);
            if (objectNumber != 0) visiting.erase(objectNumber);
        }

        out.push_back(std::move(node));
    }
};

void flattenInto(const StructNode& node, std::vector<const StructNode*>& out) {
    out.push_back(&node);
    for (const StructNode& child : node.children) flattenInto(child, out);
}

}  // namespace

const char* describe(StructTreeStatus status) noexcept {
    switch (status) {
        case StructTreeStatus::Ok:            return "ok";
        case StructTreeStatus::NoStructTree:  return "no struct tree";
        case StructTreeStatus::Malformed:     return "malformed struct tree";
        case StructTreeStatus::Truncated:     return "struct tree truncated";
    }
    return "unknown";
}

std::int32_t StructNode::minMcid() const noexcept {
    std::int32_t best = -1;
    for (const std::int32_t mcid : mcids) {
        if (best < 0 || mcid < best) best = mcid;
    }
    for (const StructNode& child : children) {
        const std::int32_t childBest = child.minMcid();
        if (childBest >= 0 && (best < 0 || childBest < best)) best = childBest;
    }
    return best;
}

bool StructNode::needsAlternateText() const noexcept {
    // 這份清單來自 ISO 32000-1 §14.8.4：非文字內容的標準結構型別。
    // /Link 也在內——沒有描述的連結在螢幕閱讀器上只會念出「連結」。
    return type == "Figure" || type == "Formula" || type == "Form" || type == "Link";
}

StructTree readStructTree(const PdfSourceDocument& source) {
    StructTree tree;

    const PdfObject* rootRef = source.trailer().find("Root");
    if (rootRef == nullptr) {
        tree.status = StructTreeStatus::NoStructTree;
        tree.diagnostic = "trailer 沒有 /Root";
        return tree;
    }
    const PdfObject catalog = source.resolve(*rootRef);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) {
        tree.status = StructTreeStatus::Malformed;
        tree.diagnostic = "/Root 不是字典";
        return tree;
    }

    // /MarkInfo /Marked。先讀，因為即使結構樹讀失敗，這個旗標仍然是
    // 「作者有沒有打算做標籤化」的證據。
    if (const PdfObject* markInfo = catalogDict->find("MarkInfo"); markInfo != nullptr) {
        const PdfObject resolvedInfo = source.resolve(*markInfo);
        if (const PdfDictionary* infoDict = resolvedInfo.asDictionary()) {
            if (const PdfObject* marked = infoDict->find("Marked"); marked != nullptr) {
                const PdfObject resolvedMarked = source.resolve(*marked);
                if (const auto* flag = std::get_if<bool>(&resolvedMarked.value())) {
                    tree.markedContent = *flag;
                }
            }
        }
    }

    const PdfObject* structRef = catalogDict->find("StructTreeRoot");
    if (structRef == nullptr) {
        // 這是「這份文件沒有標籤結構」的權威判定，也是 UI 必須明說的那一句。
        tree.status = StructTreeStatus::NoStructTree;
        tree.diagnostic = "catalog 沒有 /StructTreeRoot";
        return tree;
    }

    const PdfObject structRoot = source.resolve(*structRef);
    const PdfDictionary* structDict = structRoot.asDictionary();
    if (structDict == nullptr) {
        tree.status = StructTreeStatus::Malformed;
        tree.diagnostic = "/StructTreeRoot 不是字典";
        return tree;
    }

    const PdfObject* kids = structDict->find("K");
    if (kids == nullptr) {
        // 有根但沒有 /K：結構上合法（空的結構樹），但對使用者而言等同未標籤。
        // 標成 Malformed 而不是 NoStructTree，因為兩者的修復方式不同：
        // 前者要補內容，後者要整份重新標籤。
        tree.status = StructTreeStatus::Malformed;
        tree.diagnostic = "/StructTreeRoot 沒有 /K";
        return tree;
    }

    const std::unordered_map<int, std::int32_t> pageIndex = buildPageIndexMap(source);
    Walker walker{source, pageIndex, {}, 0, false};
    if (structRef->isRef()) walker.visiting.insert(structRef->asRef().number);
    walker.expand(*kids, 0, tree.roots, nullptr);

    tree.nodeCount = walker.nodeCount;
    if (tree.roots.empty()) {
        tree.status = StructTreeStatus::Malformed;
        tree.diagnostic = "/K 底下沒有任何結構元素";
        return tree;
    }
    if (walker.truncated) {
        tree.status = StructTreeStatus::Truncated;
        tree.diagnostic = "結構樹超過走訪上限，只顯示前一段";
        return tree;
    }

    tree.status = StructTreeStatus::Ok;
    return tree;
}

std::vector<const StructNode*> flatten(const StructTree& tree) {
    std::vector<const StructNode*> out;
    out.reserve(tree.nodeCount);
    for (const StructNode& root : tree.roots) flattenInto(root, out);
    return out;
}

}  // namespace alioth::engine::objects
