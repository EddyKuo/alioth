#include "engine/layers/ocg_reader.h"

#include <string>
#include <variant>
#include <vector>

#include "engine/bookmarks/destination_codec.h"
#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"
#include "platform/shared_file.h"

namespace alioth::engine::layers {

namespace {

using domain::OcgLayer;
using domain::OcgTree;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfSourceDocument;

// /Order 的巢狀深度上限。PDF 是不可信任輸入，惡意或損毀的檔案可以把陣列
// 疊到任意深——迭代加上限而不是遞迴，是本專案對走訪未信任結構的一貫規則
// （見 docs/SDD.md §7 與 bookmarks::outline_reader 的先例）。
constexpr int kMaxOrderDepth = 16;

std::int32_t indexOfObjectNumber(const OcgTree& tree, std::int32_t objectNumber) {
    for (std::size_t i = 0; i < tree.layers.size(); ++i) {
        if (tree.layers[i].objectNumber == objectNumber) return static_cast<std::int32_t>(i);
    }
    return -1;
}

// /Order 陣列的走訪框架：目前處理到 array 的哪個位置、掛在哪個父節點下、
// 以及「前一個同層級的葉節點」——緊接在一個 OCG 參照之後的巢狀陣列，
// 依 ISO 32000-1 §8.11.4.3 代表的是該 OCG 的子項，不是另一個獨立分組。
struct OrderFrame {
    const PdfArray* array{nullptr};
    std::size_t position{0};
    std::int32_t parent{-1};    // -1 表示掛在樹根
    std::int32_t lastLeaf{-1};  // 本層最近一個 OCG 節點的索引
};

void attachToParent(OcgTree& tree, std::int32_t parent, std::int32_t child) {
    if (parent < 0) {
        tree.roots.push_back(child);
    } else {
        tree.layers[static_cast<std::size_t>(parent)].children.push_back(child);
    }
}

void walkOrder(OcgTree& tree, const PdfArray& order) {
    std::vector<OrderFrame> stack;
    stack.push_back(OrderFrame{&order, 0, -1, -1});

    while (!stack.empty()) {
        if (static_cast<int>(stack.size()) > kMaxOrderDepth) {
            stack.pop_back();
            continue;
        }
        OrderFrame& frame = stack.back();
        if (frame.position >= frame.array->size()) {
            stack.pop_back();
            continue;
        }
        const PdfObject& entry = (*frame.array)[frame.position];
        ++frame.position;

        if (entry.isRef()) {
            const std::int32_t index = indexOfObjectNumber(tree, entry.asRef().number);
            if (index < 0) continue;  // /Order 引用了不在 /OCGs 裡的物件：忽略，不猜測
            attachToParent(tree, frame.parent, index);
            frame.lastLeaf = index;
            continue;
        }

        const PdfArray* nested = entry.asArray();
        if (nested == nullptr || nested->empty()) continue;

        const PdfObject& first = (*nested)[0];
        if (const auto* headingText = std::get_if<objects::PdfString>(&first.value())) {
            // 第一個元素是文字字串：純分組標題，本身不是 OCG，不能勾選，
            // 之後的元素是它的子項。
            OcgLayer heading;
            heading.objectNumber = -(static_cast<std::int32_t>(tree.layers.size()) + 1);
            heading.name = bookmarks::decodeTextString(*headingText);
            heading.isGroupHeading = true;
            heading.visible = true;
            const auto headingIndex = static_cast<std::int32_t>(tree.layers.size());
            tree.layers.push_back(heading);
            attachToParent(tree, frame.parent, headingIndex);
            stack.push_back(OrderFrame{nested, 1, headingIndex, -1});
        } else {
            // 第一個元素不是字串：這個巢狀陣列是「前一個 OCG 的子項」列表。
            const std::int32_t parent = frame.lastLeaf;
            stack.push_back(OrderFrame{nested, 0, parent, -1});
        }
    }
}

void applyVisibilityDefaults(OcgTree& tree, const PdfDictionary& defaultConfig) {
    const PdfObject* baseState = defaultConfig.find("BaseState");
    const bool defaultOff = baseState != nullptr && baseState->isName("OFF");
    if (defaultOff) {
        for (auto& layer : tree.layers) {
            if (!layer.isGroupHeading) layer.visible = false;
        }
    }
    auto applyList = [&](const char* key, bool visible) {
        const PdfObject* listObj = defaultConfig.find(key);
        if (listObj == nullptr) return;
        const PdfArray* list = listObj->asArray();
        if (list == nullptr) return;
        for (const auto& item : *list) {
            if (!item.isRef()) continue;
            if (auto* layer = tree.find(item.asRef().number)) layer->visible = visible;
        }
    };
    applyList("ON", true);
    applyList("OFF", false);

    const PdfObject* lockedObj = defaultConfig.find("Locked");
    if (const PdfArray* locked = lockedObj != nullptr ? lockedObj->asArray() : nullptr) {
        for (const auto& item : *locked) {
            if (!item.isRef()) continue;
            if (auto* layer = tree.find(item.asRef().number)) layer->locked = true;
        }
    }
}

void applyRadioGroups(OcgTree& tree, const PdfArray& rbGroups) {
    std::int32_t groupId = 0;
    for (const auto& groupEntry : rbGroups) {
        const PdfArray* group = groupEntry.asArray();
        if (group == nullptr) continue;
        bool used = false;
        for (const auto& item : *group) {
            if (!item.isRef()) continue;
            if (auto* layer = tree.find(item.asRef().number)) {
                layer->radioGroup = groupId;
                used = true;
            }
        }
        if (used) ++groupId;
    }
}

}  // namespace

domain::OcgTree readOcgTree(const std::string& bytes) {
    domain::OcgTree tree;
    if (bytes.empty()) return tree;

    PdfSourceDocument source;
    if (source.open(bytes) != objects::SourceStatus::Ok) return tree;

    const PdfObject* rootRef = source.trailer().find("Root");
    if (rootRef == nullptr) return tree;
    const PdfObject catalogObj = source.resolve(*rootRef);
    const PdfDictionary* catalog = catalogObj.asDictionary();
    if (catalog == nullptr) return tree;

    const PdfObject* propsRef = catalog->find("OCProperties");
    if (propsRef == nullptr) return tree;
    const PdfObject propsObj = source.resolve(*propsRef);
    const PdfDictionary* props = propsObj.asDictionary();
    if (props == nullptr) return tree;

    const PdfObject* ocgsRef = props->find("OCGs");
    const PdfArray* ocgs = ocgsRef != nullptr ? ocgsRef->asArray() : nullptr;
    if (ocgs == nullptr) return tree;  // /OCProperties 存在但沒有任何圖層，視為無圖層

    tree.present = true;
    for (const auto& item : *ocgs) {
        if (!item.isRef()) continue;
        const PdfObject ocgObj = source.resolve(item);
        const PdfDictionary* ocgDict = ocgObj.asDictionary();
        if (ocgDict == nullptr) continue;
        OcgLayer layer;
        layer.objectNumber = item.asRef().number;
        if (const PdfObject* nameObj = ocgDict->find("Name")) {
            if (const auto* str = std::get_if<objects::PdfString>(&nameObj->value())) {
                layer.name = bookmarks::decodeTextString(*str);
            }
        }
        tree.layers.push_back(std::move(layer));
    }

    if (const PdfObject* defaultRef = props->find("D")) {
        const PdfObject defaultObj = source.resolve(*defaultRef);
        if (const PdfDictionary* defaultConfig = defaultObj.asDictionary()) {
            applyVisibilityDefaults(tree, *defaultConfig);

            if (const PdfObject* orderObj = defaultConfig->find("Order")) {
                if (const PdfArray* order = orderObj->asArray()) walkOrder(tree, *order);
            }
            if (const PdfObject* rbObj = defaultConfig->find("RBGroups")) {
                if (const PdfArray* rb = rbObj->asArray()) applyRadioGroups(tree, *rb);
            }
        }
    }

    // /Order 沒提到的 OCG 依規範附加在面板最後（未分組）。
    for (std::size_t i = 0; i < tree.layers.size(); ++i) {
        const auto index = static_cast<std::int32_t>(i);
        const bool placed = [&] {
            for (const auto root : tree.roots) {
                if (root == index) return true;
            }
            for (const auto& layer : tree.layers) {
                for (const auto child : layer.children) {
                    if (child == index) return true;
                }
            }
            return false;
        }();
        if (!placed) tree.roots.push_back(index);
    }

    return tree;
}

domain::OcgTree loadOcgTree(const std::string& utf8Path) {
    platform::SharedReadFile file;
    if (!file.open(utf8Path)) return {};
    const std::uint64_t size = file.size();
    std::string bytes(static_cast<std::size_t>(size), '\0');
    const std::size_t read = file.read(0, bytes.data(), bytes.size());
    if (read != bytes.size()) return {};
    return readOcgTree(bytes);
}

}  // namespace alioth::engine::layers
