#include "engine/redaction/pdf_document_rewriter.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace alioth::engine::redaction {
namespace {

// PDF 是不可信任輸入：走訪一律迭代並設上限。互相指涉的物件在真實檔案裡
// 就存在（/Length 指回自己那類），遞迴版本會直接爆堆疊。
constexpr int kMaxObjects = 2'000'000;
constexpr int kMaxDepth = 256;

// 走訪一個物件裡的所有間接參照。以顯式堆疊實作，深度上限同樣是防惡意輸入。
template <typename Fn>
void forEachRef(const PdfObject& root, Fn&& visit) {
    struct Frame {
        const PdfObject* object;
        int depth;
    };
    std::vector<Frame> stack{{&root, 0}};
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.depth > kMaxDepth) continue;

        const PdfObject& current = *frame.object;
        if (current.isRef()) {
            visit(current.asRef());
            continue;
        }
        if (const objects::PdfArray* array = current.asArray()) {
            for (const PdfObject& item : *array) stack.push_back({&item, frame.depth + 1});
            continue;
        }
        // 串流也有字典，兩種形態都要走進去。
        if (const PdfDictionary* dict = current.asDictionary()) {
            for (const auto& entry : dict->entries()) {
                stack.push_back({&entry.second, frame.depth + 1});
            }
        }
    }
}

// trailer 只保留跨版本仍然有意義的鍵。原檔若是 xref 串流，trailer_ 會混進
// /Type /W /Index /Filter /Length 這些描述**那個串流自己**的鍵；照抄到新的
// 傳統 trailer 裡會產生一份自相矛盾的字典，部分解析器直接拒絕開啟。
[[nodiscard]] bool isTrailerKeyKept(const std::string& key) {
    return key == "Root" || key == "Info" || key == "ID";
}

}  // namespace

SourceStatus PdfDocumentRewriter::open(std::string bytes, std::string* diagnostic) {
    const SourceStatus status = source_.open(std::move(bytes), diagnostic);
    if (status != SourceStatus::Ok) return status;

    // 檔頭沿用原檔：版本號降級會讓原本合法的結構（例如 1.5 起的物件串流語意）
    // 在嚴格解析器上變成警告。
    const std::string& raw = source_.bytes();
    const std::size_t lineEnd = raw.find_first_of("\r\n");
    header_ = lineEnd == std::string::npos ? std::string{"%PDF-1.7"} : raw.substr(0, lineEnd);

    trailer_ = PdfDictionary{};
    for (const auto& entry : source_.trailer().entries()) {
        if (isTrailerKeyKept(entry.first)) trailer_.set(entry.first, entry.second);
    }

    nextNumber_ = static_cast<int>(source_.trailerSize());
    if (nextNumber_ < 1) nextNumber_ = 1;

    loadReachable();
    opened_ = true;
    return SourceStatus::Ok;
}

void PdfDocumentRewriter::loadReachable() {
    std::vector<int> queue;
    std::set<int> seen;
    for (const auto& entry : trailer_.entries()) {
        forEachRef(entry.second, [&](const PdfRef& ref) {
            if (ref.valid() && seen.insert(ref.number).second) queue.push_back(ref.number);
        });
    }

    int budget = kMaxObjects;
    while (!queue.empty() && budget-- > 0) {
        const int number = queue.back();
        queue.pop_back();
        if (!source_.hasObject(number)) continue;

        PdfObject loaded = source_.object(number);
        forEachRef(loaded, [&](const PdfRef& ref) {
            if (ref.valid() && seen.insert(ref.number).second) queue.push_back(ref.number);
        });
        generations_[number] = source_.generationOf(number);
        objects_.emplace(number, std::move(loaded));
        if (number >= nextNumber_) nextNumber_ = number + 1;
    }
}

bool PdfDocumentRewriter::pageRef(int index, PdfRef& out) const {
    const std::vector<PdfRef>& pages = source_.pages();
    if (index < 0 || index >= static_cast<int>(pages.size())) return false;
    out = pages[static_cast<std::size_t>(index)];
    return true;
}

const PdfObject* PdfDocumentRewriter::object(int number) const {
    const auto it = objects_.find(number);
    return it == objects_.end() ? nullptr : &it->second;
}

PdfObject* PdfDocumentRewriter::object(int number) {
    const auto it = objects_.find(number);
    return it == objects_.end() ? nullptr : &it->second;
}

PdfObject PdfDocumentRewriter::resolve(const PdfObject& value) const {
    if (!value.isRef()) return value;
    const PdfObject* found = object(value.asRef().number);
    return found == nullptr ? PdfObject{} : *found;
}

PdfObject PdfDocumentRewriter::inheritedPageAttribute(const PdfRef& page,
                                                      const std::string& key) const {
    PdfRef current = page;
    std::set<int> visited;
    for (int depth = 0; depth < kMaxDepth; ++depth) {
        if (!visited.insert(current.number).second) break;
        const PdfObject* node = object(current.number);
        if (node == nullptr) break;
        const PdfDictionary* dict = node->asDictionary();
        if (dict == nullptr) break;
        if (const PdfObject* value = dict->find(key)) return *value;
        const PdfObject* parent = dict->find("Parent");
        if (parent == nullptr || !parent->isRef()) break;
        current = parent->asRef();
    }
    return PdfObject{};
}

void PdfDocumentRewriter::setObject(int number, PdfObject value) {
    objects_[number] = std::move(value);
    generations_.emplace(number, 0);
    if (number >= nextNumber_) nextNumber_ = number + 1;
}

int PdfDocumentRewriter::addObject(PdfObject value) {
    const int number = nextNumber_++;
    objects_[number] = std::move(value);
    generations_[number] = 0;
    return number;
}

int PdfDocumentRewriter::referenceCount(int number) const {
    int count = 0;
    for (const auto& entry : trailer_.entries()) {
        forEachRef(entry.second, [&](const PdfRef& ref) {
            if (ref.number == number) ++count;
        });
    }
    for (const auto& [objectNumber, value] : objects_) {
        (void)objectNumber;
        forEachRef(value, [&](const PdfRef& ref) {
            if (ref.number == number) ++count;
        });
    }
    return count;
}

std::vector<int> PdfDocumentRewriter::reachableObjects() const {
    std::vector<int> queue;
    std::set<int> seen;
    for (const auto& entry : trailer_.entries()) {
        forEachRef(entry.second, [&](const PdfRef& ref) {
            if (ref.valid() && seen.insert(ref.number).second) queue.push_back(ref.number);
        });
    }

    std::vector<int> result;
    int budget = kMaxObjects;
    while (!queue.empty() && budget-- > 0) {
        const int number = queue.back();
        queue.pop_back();
        const auto it = objects_.find(number);
        if (it == objects_.end()) continue;  // 已被移除：不寫出去，也不留下懸空參照
        result.push_back(number);
        forEachRef(it->second, [&](const PdfRef& ref) {
            if (ref.valid() && seen.insert(ref.number).second) queue.push_back(ref.number);
        });
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string PdfDocumentRewriter::build() const {
    const std::vector<int> live = reachableObjects();

    std::string out = header_;
    out += '\n';
    // 二進位註解行：讓傳輸工具把檔案判定為二進位而不是文字，
    // 少了它，經過會做換行正規化的通道時串流資料會被靜默改壞。
    out += "%\xE2\xE3\xCF\xD3\n";

    std::map<int, std::size_t> offsets;
    for (const int number : live) {
        const auto generation = generations_.find(number);
        offsets[number] = out.size();
        out += objects::serializeIndirect(
            number, generation == generations_.end() ? 0 : generation->second,
            objects_.at(number));
    }

    const int maxNumber = live.empty() ? 0 : live.back();
    const std::size_t xrefOffset = out.size();

    // 傳統 xref 表以子區段表達不連續的編號。把缺號一律補成 free 項也可以，
    // 但那會宣告一堆並不存在的物件，qpdf 會為此發出警告，而判定標準是零警告。
    out += "xref\n";
    std::size_t index = 0;
    // 物件 0 恆為 free 串列的頭，且必須是第一個子區段。
    out += "0 1\n0000000000 65535 f \n";
    while (index < live.size()) {
        std::size_t end = index + 1;
        while (end < live.size() && live[end] == live[end - 1] + 1) ++end;
        out += std::to_string(live[index]);
        out += ' ';
        out += std::to_string(end - index);
        out += '\n';
        for (std::size_t i = index; i < end; ++i) {
            std::string offset = std::to_string(offsets.at(live[i]));
            offset.insert(offset.begin(), 10 - std::min<std::size_t>(10, offset.size()), '0');
            const auto generation = generations_.find(live[i]);
            std::string gen =
                std::to_string(generation == generations_.end() ? 0 : generation->second);
            gen.insert(gen.begin(), 5 - std::min<std::size_t>(5, gen.size()), '0');
            out += offset;
            out += ' ';
            out += gen;
            out += " n \n";
        }
        index = end;
    }

    PdfDictionary trailer;
    trailer.set("Size", PdfObject{static_cast<std::int64_t>(maxNumber + 1)});
    for (const auto& entry : trailer_.entries()) {
        if (isTrailerKeyKept(entry.first)) trailer.set(entry.first, entry.second);
    }

    out += "trailer\n";
    out += objects::serialize(PdfObject{trailer});
    out += "\nstartxref\n";
    out += std::to_string(xrefOffset);
    out += "\n%%EOF\n";
    return out;
}

namespace {

// 取得某個字典鍵底下的字典，必要時以寫入時複製確保「改它不會影響別人」。
// 回傳可直接修改的指標；無法取得時回傳 nullptr。
PdfDictionary* ensureUnsharedChildDictionary(PdfDocumentRewriter& document, PdfDictionary& owner,
                                             const std::string& key, bool createIfMissing) {
    PdfObject* slot = owner.find(key);
    if (slot == nullptr) {
        if (!createIfMissing) return nullptr;
        owner.set(key, PdfObject{PdfDictionary{}});
        slot = owner.find(key);
        return slot == nullptr ? nullptr : slot->asDictionary();
    }
    if (!slot->isRef()) return slot->asDictionary();

    const int number = slot->asRef().number;
    PdfObject* target = document.object(number);
    if (target == nullptr) return nullptr;
    if (document.referenceCount(number) <= 1) return target->asDictionary();

    const int clone = document.addObject(*target);
    owner.set(key, objects::makeRef(clone));
    PdfObject* cloned = document.object(clone);
    return cloned == nullptr ? nullptr : cloned->asDictionary();
}

}  // namespace

bool removeResourceEntries(PdfDocumentRewriter& document, int ownerObject,
                           const std::string& category, const std::vector<std::string>& names) {
    if (names.empty()) return true;
    PdfObject* owner = document.object(ownerObject);
    if (owner == nullptr) return false;
    PdfDictionary* ownerDict = owner->asDictionary();
    if (ownerDict == nullptr) return false;

    if (!ownerDict->has("Resources")) {
        const PdfObject inherited =
            document.inheritedPageAttribute(PdfRef{ownerObject, 0}, "Resources");
        const PdfObject resolved = document.resolve(inherited);
        if (const PdfDictionary* dict = resolved.asDictionary()) {
            ownerDict->set("Resources", PdfObject{*dict});
        } else {
            return true;  // 根本沒有資源字典，也就沒有東西要刪
        }
    }

    PdfDictionary* resources = ensureUnsharedChildDictionary(document, *ownerDict, "Resources", false);
    if (resources == nullptr) return false;
    PdfDictionary* group = ensureUnsharedChildDictionary(document, *resources, category, false);
    if (group == nullptr) return true;

    for (const std::string& name : names) group->remove(name);
    return true;
}

bool setResourceEntry(PdfDocumentRewriter& document, int ownerObject, const std::string& category,
                      const std::string& name, PdfObject value) {
    PdfObject* owner = document.object(ownerObject);
    if (owner == nullptr) return false;
    PdfDictionary* ownerDict = owner->asDictionary();
    if (ownerDict == nullptr) return false;

    if (!ownerDict->has("Resources")) {
        // 先把繼承來的整份複製下來，否則新建的 /Resources 會遮蔽繼承鏈。
        const PdfObject inherited =
            document.inheritedPageAttribute(PdfRef{ownerObject, 0}, "Resources");
        const PdfObject resolved = document.resolve(inherited);
        if (const PdfDictionary* dict = resolved.asDictionary()) {
            ownerDict->set("Resources", PdfObject{*dict});
        } else {
            ownerDict->set("Resources", PdfObject{PdfDictionary{}});
        }
    }

    PdfDictionary* resources = ensureUnsharedChildDictionary(document, *ownerDict, "Resources", true);
    if (resources == nullptr) return false;
    PdfDictionary* group = ensureUnsharedChildDictionary(document, *resources, category, true);
    if (group == nullptr) return false;
    group->set(name, std::move(value));
    return true;
}

objects::PdfArray* unsharedDictionaryArray(PdfDocumentRewriter& document, int ownerObject,
                                           const std::string& key) {
    PdfObject* owner = document.object(ownerObject);
    if (owner == nullptr) return nullptr;
    PdfDictionary* ownerDict = owner->asDictionary();
    if (ownerDict == nullptr) return nullptr;
    PdfObject* slot = ownerDict->find(key);
    if (slot == nullptr) return nullptr;
    if (!slot->isRef()) return slot->asArray();

    const int number = slot->asRef().number;
    PdfObject* target = document.object(number);
    if (target == nullptr) return nullptr;
    if (document.referenceCount(number) <= 1) return target->asArray();

    const int clone = document.addObject(*target);
    ownerDict->set(key, objects::makeRef(clone));
    PdfObject* cloned = document.object(clone);
    return cloned == nullptr ? nullptr : cloned->asArray();
}

}  // namespace alioth::engine::redaction
