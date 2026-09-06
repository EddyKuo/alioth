#include "engine/pageops/compose_document.h"

#include <algorithm>
#include <set>

#include "engine/objects/pdf_parser.h"

namespace alioth::engine::pageops {
namespace {

using domain::RectF;

// 可繼承的頁面屬性（ISO 32000-2 表 30）。攤平之前要全部下推，
// 否則中間的 /Pages 節點被丟掉之後，那些頁面就失去字型與尺寸。
constexpr const char* kInheritable[] = {"Resources", "MediaBox", "CropBox", "Rotate"};

// /MediaBox 缺失時的退路。規格說它必填，但真實檔案不見得照做，
// 而沒有框就算不出任何版面。US Letter 是最不會讓人誤以為「這是刻意設計」的尺寸。
constexpr double kFallbackWidth = 612.0;
constexpr double kFallbackHeight = 792.0;

}  // namespace

const char* describe(PageOpsStatus status) noexcept {
    switch (status) {
        case PageOpsStatus::Ok: return "ok";
        case PageOpsStatus::SourceInvalid: return "主文件無法開啟或已加密";
        case PageOpsStatus::SecondaryInvalid: return "第二份文件無法開啟或已加密";
        case PageOpsStatus::PageOutOfRange: return "頁碼超出範圍";
        case PageOpsStatus::InvalidRequest: return "請求本身不成立";
        case PageOpsStatus::ContentUnreadable: return "內容串流無法解開";
        case PageOpsStatus::LayoutRejected: return "版面驗證未通過";
    }
    return "unknown";
}

PageOpsStatus ComposeDocument::open(std::string bytes, std::string* diagnostic) {
    const objects::SourceStatus status = document_.open(std::move(bytes), diagnostic);
    if (status != objects::SourceStatus::Ok) {
        if (diagnostic != nullptr && diagnostic->empty()) *diagnostic = objects::describe(status);
        return PageOpsStatus::SourceInvalid;
    }
    flatten();
    opened_ = true;
    return PageOpsStatus::Ok;
}

void ComposeDocument::flatten() {
    pages_ = document_.source().pages();

    // 根 /Pages 節點：從 /Root /Pages 走。取不到就自己建一個，
    // 因為後面所有操作都需要一個可以掛頁面的地方。
    PdfRef root{};
    const PdfObject* rootEntry = document_.trailer().find("Root");
    if (rootEntry != nullptr) {
        const PdfObject catalog = document_.resolve(*rootEntry);
        if (const PdfDictionary* dict = catalog.asDictionary()) {
            if (const PdfObject* pages = dict->find("Pages")) {
                if (pages->isRef()) root = pages->asRef();
            }
        }
    }
    if (!root.valid() || document_.object(root.number) == nullptr) {
        PdfDictionary node;
        node.set("Type", objects::makeName("Pages"));
        root = PdfRef{document_.addObject(PdfObject{std::move(node)}), 0};
        if (rootEntry != nullptr) {
            PdfObject catalogValue = *rootEntry;
            if (catalogValue.isRef()) {
                if (PdfObject* catalog = document_.object(catalogValue.asRef().number)) {
                    if (PdfDictionary* dict = catalog->asDictionary()) {
                        dict->set("Pages", objects::makeRef(root.number));
                    }
                }
            }
        }
    }
    root_ = root;

    // 先把繼承屬性下推，再改 /Parent：順序反過來的話繼承鏈已經被切斷，
    // 查到的會是新的根節點而不是原本那層。
    for (const PdfRef& page : pages_) {
        PdfObject* object = document_.object(page.number);
        if (object == nullptr) continue;
        PdfDictionary* dict = object->asDictionary();
        if (dict == nullptr) continue;
        for (const char* key : kInheritable) {
            if (dict->has(key)) continue;
            const PdfObject inherited = document_.inheritedPageAttribute(page, key);
            if (!inherited.isNull()) dict->set(key, inherited);
        }
    }
    setPages(pages_);
}

bool ComposeDocument::pageAt(int index, PdfRef& out) const {
    if (index < 0 || index >= static_cast<int>(pages_.size())) return false;
    out = pages_[static_cast<std::size_t>(index)];
    return true;
}

void ComposeDocument::setPages(std::vector<PdfRef> pages) {
    pages_ = std::move(pages);

    PdfArray kids;
    kids.reserve(pages_.size());
    for (const PdfRef& page : pages_) kids.push_back(objects::makeRef(page.number, page.generation));

    if (PdfObject* rootObject = document_.object(root_.number)) {
        if (PdfDictionary* dict = rootObject->asDictionary()) {
            dict->set("Type", objects::makeName("Pages"));
            dict->set("Kids", PdfObject{std::move(kids)});
            dict->set("Count", PdfObject{static_cast<std::int64_t>(pages_.size())});
            // 攤平後的根節點不該再帶繼承屬性：它們已經下推到每一頁，
            // 留著只會在日後有人加頁時產生兩個真相。
            for (const char* key : kInheritable) dict->remove(key);
        }
    }

    // 同一個頁面物件可能在清單裡出現兩次（複製頁），/Parent 設同一個值不受影響。
    for (const PdfRef& page : pages_) {
        if (PdfObject* object = document_.object(page.number)) {
            if (PdfDictionary* dict = object->asDictionary()) {
                dict->set("Type", objects::makeName("Page"));
                dict->set("Parent", objects::makeRef(root_.number, root_.generation));
            }
        }
    }
}

RectF ComposeDocument::mediaBox(const PdfRef& page) const {
    const PdfDictionary* dict = pageDictionary(document_, page);
    RectF box{0.0, 0.0, kFallbackWidth, kFallbackHeight};
    if (dict == nullptr) return box;
    if (const PdfObject* value = dict->find("MediaBox")) {
        RectF parsed{};
        if (readRectArray(document_, *value, parsed) && !parsed.isEmpty()) return parsed;
    }
    return box;
}

RectF ComposeDocument::cropBox(const PdfRef& page) const {
    const RectF media = mediaBox(page);
    const PdfDictionary* dict = pageDictionary(document_, page);
    if (dict == nullptr) return media;
    const PdfObject* value = dict->find("CropBox");
    if (value == nullptr) return media;
    RectF parsed{};
    if (!readRectArray(document_, *value, parsed)) return media;
    const RectF clipped = parsed.intersected(media);
    return clipped.isEmpty() ? media : clipped;
}

int ComposeDocument::rotation(const PdfRef& page) const {
    const PdfDictionary* dict = pageDictionary(document_, page);
    if (dict == nullptr) return 0;
    const PdfObject* value = dict->find("Rotate");
    if (value == nullptr) return 0;
    const PdfObject resolved = document_.resolve(*value);
    const int degrees = static_cast<int>(resolved.asInteger(0));
    // /Rotate 規格上必須是 90 的倍數；不是的話當成 0 而不是四捨五入，
    // 猜錯 90 度比不轉更難被發現。
    return degrees % 90 == 0 ? ((degrees % 360) + 360) % 360 : 0;
}

PdfDictionary* pageDictionary(PdfDocumentRewriter& document, const PdfRef& page) {
    PdfObject* object = document.object(page.number);
    return object == nullptr ? nullptr : object->asDictionary();
}

const PdfDictionary* pageDictionary(const PdfDocumentRewriter& document, const PdfRef& page) {
    const PdfObject* object = document.object(page.number);
    return object == nullptr ? nullptr : object->asDictionary();
}

ContentBytes readPageContent(const PdfDocumentRewriter& document, const PdfRef& page) {
    ContentBytes out;
    const PdfDictionary* dict = pageDictionary(document, page);
    if (dict == nullptr) {
        out.diagnostic = "頁面不是字典";
        return out;
    }
    const PdfObject* contents = dict->find("Contents");
    if (contents == nullptr) {
        // 空白頁沒有 /Contents，這是合法的，不是錯誤。
        out.ok = true;
        return out;
    }

    std::vector<PdfObject> streams;
    const PdfObject resolved = document.resolve(*contents);
    if (const PdfArray* array = resolved.asArray()) {
        for (const PdfObject& item : *array) streams.push_back(document.resolve(item));
    } else {
        streams.push_back(resolved);
    }

    const auto resolver = [&document](const PdfRef& ref) { return document.resolve(PdfObject{ref}); };
    for (const PdfObject& item : streams) {
        const objects::PdfStream* stream = item.asStream();
        if (stream == nullptr) continue;  // 懸空參照：跳過而不是整份失敗
        const objects::DecodeResult decoded = objects::decodeStream(*stream, resolver);
        if (!decoded.ok) {
            out.diagnostic = decoded.diagnostic;
            return out;
        }
        if (!out.data.empty()) out.data += '\n';
        out.data += decoded.data;
    }
    out.ok = true;
    return out;
}

int addContentStream(PdfDocumentRewriter& document, std::string data) {
    objects::PdfStream stream;
    stream.data = std::move(data);
    return document.addObject(PdfObject{std::move(stream)});
}

void setPageContents(PdfDocumentRewriter& document, const PdfRef& page,
                     const std::vector<int>& contentObjects) {
    PdfDictionary* dict = pageDictionary(document, page);
    if (dict == nullptr) return;
    if (contentObjects.empty()) {
        dict->remove("Contents");
        return;
    }
    if (contentObjects.size() == 1) {
        dict->set("Contents", objects::makeRef(contentObjects.front()));
        return;
    }
    PdfArray array;
    array.reserve(contentObjects.size());
    for (const int number : contentObjects) array.push_back(objects::makeRef(number));
    dict->set("Contents", PdfObject{std::move(array)});
}

std::vector<int> wrapPageContentInGraphicsState(PdfDocumentRewriter& document,
                                                const PdfRef& page) {
    PdfDictionary* dict = pageDictionary(document, page);
    if (dict == nullptr) return {};
    const PdfObject* contents = dict->find("Contents");
    if (contents == nullptr) {
        // 空白頁：仍然回傳一個可以往上疊的空序列，呼叫端不必分兩種情況。
        return {};
    }

    // 不解開原內容，只在前後各加一個極小的串流。解開再重寫會讓原本壓縮過的
    // 內容整份變成未壓縮，檔案可能因此大好幾倍，而我們要的只是 q 與 Q。
    std::vector<int> objectsOut;
    objectsOut.push_back(addContentStream(document, "q\n"));

    const PdfObject resolved = document.resolve(*contents);
    if (const PdfArray* array = resolved.asArray()) {
        for (const PdfObject& item : *array) {
            if (item.isRef()) objectsOut.push_back(item.asRef().number);
        }
    } else if (contents->isRef()) {
        objectsOut.push_back(contents->asRef().number);
    } else {
        // 直接內嵌的串流（少見但合法）：搬成獨立物件才能排進陣列。
        if (const objects::PdfStream* stream = resolved.asStream()) {
            objectsOut.push_back(document.addObject(PdfObject{*stream}));
        }
    }

    objectsOut.push_back(addContentStream(document, "\nQ\n"));
    setPageContents(document, page, objectsOut);
    return objectsOut;
}

std::string formatMatrix(const domain::compose::Matrix& matrix) {
    std::string out = objects::formatReal(matrix.a);
    out += ' ';
    out += objects::formatReal(matrix.b);
    out += ' ';
    out += objects::formatReal(matrix.c);
    out += ' ';
    out += objects::formatReal(matrix.d);
    out += ' ';
    out += objects::formatReal(matrix.e);
    out += ' ';
    out += objects::formatReal(matrix.f);
    return out;
}

PdfObject makeRectArray(const RectF& rect) {
    const RectF box = rect.normalized();
    return objects::makeNumberArray({box.left, box.bottom, box.right, box.top});
}

bool readRectArray(const PdfDocumentRewriter& document, const PdfObject& value, RectF& out) {
    const PdfObject resolved = document.resolve(value);
    const PdfArray* array = resolved.asArray();
    if (array == nullptr || array->size() < 4) return false;
    double numbers[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i) {
        const PdfObject item = document.resolve((*array)[static_cast<std::size_t>(i)]);
        if (!item.isNumber()) return false;
        numbers[i] = item.asNumber();
    }
    out = RectF{numbers[0], numbers[1], numbers[2], numbers[3]}.normalized();
    return true;
}

}  // namespace alioth::engine::pageops
