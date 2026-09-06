#include "engine/pageops/object_copier.h"

namespace alioth::engine::pageops {
namespace {

// 深度上限：PDF 是不可信任輸入，互相巢狀的陣列可以做得任意深。
constexpr int kMaxDepth = 128;

}  // namespace

PdfObject ObjectCopier::copyValue(const PdfObject& value) { return copyValue(value, 0); }

PdfObject ObjectCopier::copyValue(const PdfObject& value, int depth) {
    if (same_) return value;
    if (depth > kMaxDepth) return PdfObject{};

    if (value.isRef()) {
        const int mapped = copyObject(value.asRef().number);
        return mapped > 0 ? objects::makeRef(mapped) : PdfObject{};
    }
    if (const objects::PdfArray* array = value.asArray()) {
        objects::PdfArray out;
        out.reserve(array->size());
        for (const PdfObject& item : *array) out.push_back(copyValue(item, depth + 1));
        return PdfObject{std::move(out)};
    }
    if (const objects::PdfStream* stream = value.asStream()) {
        objects::PdfStream out;
        out.data = stream->data;
        for (const auto& entry : stream->dict.entries()) {
            out.dict.set(entry.first, copyValue(entry.second, depth + 1));
        }
        return PdfObject{std::move(out)};
    }
    if (const objects::PdfDictionary* dict = value.asDictionary()) {
        objects::PdfDictionary out;
        for (const auto& entry : dict->entries()) {
            out.set(entry.first, copyValue(entry.second, depth + 1));
        }
        return PdfObject{std::move(out)};
    }
    return value;
}

int ObjectCopier::copyObject(int number) {
    if (same_) return number;
    if (number <= 0) return 0;

    const auto found = map_.find(number);
    if (found != map_.end()) return found->second;

    const PdfObject* source = from_->object(number);
    if (source == nullptr) return 0;

    // 先佔號、先登記，再複製內容：來源物件若（直接或間接）指回自己，
    // 遞迴會在這一步命中 map_ 而停下來。少了這一步就是無限遞迴而不是壞資料。
    const int destination = to_->addObject(PdfObject{});
    map_[number] = destination;

    const PdfObject copied = copyValue(*source, 0);
    to_->setObject(destination, copied);
    return destination;
}

}  // namespace alioth::engine::pageops
