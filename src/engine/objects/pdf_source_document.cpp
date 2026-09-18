#include "engine/objects/pdf_source_document.h"

#include <algorithm>
#include <set>

#include "engine/objects/pdf_parser.h"

namespace alioth::engine::objects {

namespace {

// xref 鏈的長度上限。正常文件每存一次增加一段，上千段已經是異常；
// 沒有上限的話一份自我指涉的檔案會讓開檔永遠不回來。
constexpr int kMaxXrefSections = 512;
constexpr int kMaxPageTreeNodes = 200000;
constexpr int kMaxInheritDepth = 64;

[[nodiscard]] bool isWhitespace(unsigned char c) noexcept {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

// 從檔尾往回找 startxref。只掃最後 2 KB：規格要求它在檔尾，
// 全檔搜尋反而會撿到文件內容裡剛好出現的同名字串。
[[nodiscard]] bool findStartxref(const std::string& bytes, std::size_t& offset) {
    constexpr std::size_t kTailWindow = 2048;
    const std::size_t start = bytes.size() > kTailWindow ? bytes.size() - kTailWindow : 0;
    const std::size_t found = bytes.rfind("startxref");
    if (found == std::string::npos || found < start) return false;

    PdfParser parser(bytes, found + 9);
    std::string token;
    if (!parser.readKeyword(token)) return false;
    if (token.empty() || !std::all_of(token.begin(), token.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) {
        return false;
    }
    // 位移可以是任意長度的數字串（損毀或惡意的檔案），轉換必須擋住溢位。
    unsigned long long value = 0;
    try {
        value = std::stoull(token);
    } catch (...) {
        return false;
    }
    if (value >= bytes.size()) return false;
    offset = static_cast<std::size_t>(value);
    return true;
}

[[nodiscard]] std::int64_t readWidthField(const std::string& data, std::size_t& pos, int width,
                                          std::int64_t fallback) {
    if (width == 0) return fallback;
    std::int64_t value = 0;
    for (int i = 0; i < width; ++i) {
        if (pos >= data.size()) return fallback;
        value = (value << 8) | static_cast<unsigned char>(data[pos]);
        ++pos;
    }
    return value;
}

}  // namespace

const char* describe(SourceStatus status) noexcept {
    switch (status) {
        case SourceStatus::Ok: return "成功";
        case SourceStatus::Empty: return "檔案是空的";
        case SourceStatus::NotPdf: return "不是 PDF（缺少 %PDF- 標頭）";
        case SourceStatus::NoStartxref: return "找不到 startxref";
        case SourceStatus::BadXref: return "交叉參照表損毀";
        case SourceStatus::Encrypted: return "加密文件不支援物件層寫入";
        case SourceStatus::UnsupportedFilter: return "交叉參照使用了不支援的濾鏡";
    }
    return "未知狀態";
}

SourceStatus PdfSourceDocument::open(std::string bytes, std::string* diagnostic) {
    bytes_ = std::move(bytes);
    xref_.clear();
    trailer_ = PdfDictionary{};
    pages_.clear();
    cache_.clear();
    trailerSize_ = 0;
    encrypted_ = false;

    if (bytes_.empty()) return SourceStatus::Empty;
    if (bytes_.compare(0, 5, "%PDF-") != 0) return SourceStatus::NotPdf;
    if (!findStartxref(bytes_, lastXrefOffset_)) return SourceStatus::NoStartxref;

    std::string reason;
    if (!parseXrefChain(&reason)) {
        if (diagnostic != nullptr) *diagnostic = reason;
        return SourceStatus::BadXref;
    }

    // 加密文件的字串與串流必須先以文件金鑰加密才能寫入。第一版明確拒絕，
    // 而不是寫出讀不出來的內容（ADR-002 驗收條件 5）。
    encrypted_ = trailer_.has("Encrypt");
    if (encrypted_) {
        if (diagnostic != nullptr) *diagnostic = "trailer 含 /Encrypt";
        return SourceStatus::Encrypted;
    }

    if (trailerSize_ <= 0) {
        // /Size 缺失或是壞的。取號改以實際看到的最大編號加一為準，
        // 寧可讓新物件編號偏大，也不能與既有物件相撞。
        int maxNumber = 0;
        for (const auto& entry : xref_) maxNumber = std::max(maxNumber, entry.first);
        trailerSize_ = maxNumber + 1;
    }

    collectPages();
    return SourceStatus::Ok;
}

bool PdfSourceDocument::parseXrefChain(std::string* diagnostic) {
    std::set<std::size_t> visited;
    std::size_t offset = lastXrefOffset_;
    bool first = true;

    for (int section = 0; section < kMaxXrefSections; ++section) {
        if (visited.count(offset) != 0) break;  // 自我指涉的 /Prev 鏈
        visited.insert(offset);
        if (offset >= bytes_.size()) {
            if (diagnostic != nullptr) *diagnostic = "xref 位移超出檔案長度";
            return false;
        }

        std::size_t cursor = offset;
        while (cursor < bytes_.size() && isWhitespace(static_cast<unsigned char>(bytes_[cursor]))) {
            ++cursor;
        }
        const bool isTable = bytes_.compare(cursor, 4, "xref") == 0;
        if (first) style_ = isTable ? XrefStyle::Table : XrefStyle::Stream;

        std::size_t previous = 0;
        std::size_t hybrid = 0;
        bool hasPrevious = false;
        bool hasHybrid = false;
        if (isTable) {
            if (!parseXrefTable(cursor, previous, hybrid, hasPrevious, hasHybrid, diagnostic)) {
                return false;
            }
        } else if (!parseXrefStream(cursor, previous, hasPrevious, diagnostic)) {
            return false;
        }
        first = false;

        // 混合式檔案（/XRefStm）：傳統表之外還有一份 xref 串流，
        // 表裡沒有的物件才去那裡找，因此順序上排在本段之後。
        if (hasHybrid && visited.count(hybrid) == 0) {
            visited.insert(hybrid);
            std::size_t ignoredPrev = 0;
            bool ignoredHasPrev = false;
            (void)parseXrefStream(hybrid, ignoredPrev, ignoredHasPrev, nullptr);
        }

        if (!hasPrevious) break;
        offset = previous;
    }
    return !xref_.empty();
}

bool PdfSourceDocument::parseXrefTable(std::size_t offset, std::size_t& previous,
                                       std::size_t& hybrid, bool& hasPrevious, bool& hasHybrid,
                                       std::string* diagnostic) {
    PdfParser parser(bytes_, offset);
    if (!parser.consumeKeyword("xref")) {
        if (diagnostic != nullptr) *diagnostic = "xref 關鍵字缺失";
        return false;
    }

    while (true) {
        const std::size_t saved = parser.position();
        std::string token;
        if (!parser.readKeyword(token)) {
            if (diagnostic != nullptr) *diagnostic = "xref 子段落截斷";
            return false;
        }
        if (token == "trailer") break;

        parser.seek(saved);
        std::string firstToken;
        std::string countToken;
        if (!parser.readKeyword(firstToken) || !parser.readKeyword(countToken)) {
            if (diagnostic != nullptr) *diagnostic = "xref 子段落標頭損毀";
            return false;
        }
        long long firstNumber = 0;
        long long count = 0;
        try {
            firstNumber = std::stoll(firstToken);
            count = std::stoll(countToken);
        } catch (...) {
            if (diagnostic != nullptr) *diagnostic = "xref 子段落標頭不是數字";
            return false;
        }
        if (firstNumber < 0 || count < 0 || count > 50000000) {
            if (diagnostic != nullptr) *diagnostic = "xref 子段落數量不合理";
            return false;
        }

        for (long long i = 0; i < count; ++i) {
            std::string offsetToken;
            std::string generationToken;
            std::string typeToken;
            if (!parser.readKeyword(offsetToken) || !parser.readKeyword(generationToken) ||
                !parser.readKeyword(typeToken)) {
                if (diagnostic != nullptr) *diagnostic = "xref 項目截斷";
                return false;
            }
            if (typeToken != "n") continue;  // f 代表已釋放，沒有內容可讀
            ObjectLocation location{};
            try {
                location.offset = static_cast<std::size_t>(std::stoull(offsetToken));
                location.generation = static_cast<int>(std::stol(generationToken));
            } catch (...) {
                continue;
            }
            recordEntry(static_cast<int>(firstNumber + i), location);
        }
    }

    PdfObject trailerObject;
    if (!parser.parseObject(trailerObject)) {
        if (diagnostic != nullptr) *diagnostic = "trailer 字典損毀";
        return false;
    }
    const PdfDictionary* dict = trailerObject.asDictionary();
    if (dict == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "trailer 不是字典";
        return false;
    }
    mergeTrailer(*dict);

    if (const PdfObject* prev = dict->find("Prev")) {
        const std::int64_t value = prev->asInteger(-1);
        if (value >= 0 && static_cast<std::size_t>(value) < bytes_.size()) {
            previous = static_cast<std::size_t>(value);
            hasPrevious = true;
        }
    }
    if (const PdfObject* stm = dict->find("XRefStm")) {
        const std::int64_t value = stm->asInteger(-1);
        if (value >= 0 && static_cast<std::size_t>(value) < bytes_.size()) {
            hybrid = static_cast<std::size_t>(value);
            hasHybrid = true;
        }
    }
    return true;
}

bool PdfSourceDocument::parseXrefStream(std::size_t offset, std::size_t& previous,
                                        bool& hasPrevious, std::string* diagnostic) {
    PdfParser parser(bytes_, offset);
    int number = 0;
    int generation = 0;
    PdfObject object;
    if (!parser.parseIndirectObject(number, generation, object)) {
        if (diagnostic != nullptr) *diagnostic = "xref 串流物件損毀";
        return false;
    }
    const PdfStream* stream = object.asStream();
    if (stream == nullptr) {
        if (diagnostic != nullptr) *diagnostic = "startxref 指向的不是串流也不是 xref 表";
        return false;
    }

    const DecodeResult decoded = decodeStream(*stream);
    if (!decoded.ok) {
        if (diagnostic != nullptr) *diagnostic = decoded.diagnostic;
        return false;
    }

    const PdfObject* widthsObject = stream->dict.find("W");
    const PdfArray* widths = widthsObject == nullptr ? nullptr : widthsObject->asArray();
    if (widths == nullptr || widths->size() < 3) {
        if (diagnostic != nullptr) *diagnostic = "xref 串流缺少 /W";
        return false;
    }
    const int w0 = static_cast<int>((*widths)[0].asInteger(0));
    const int w1 = static_cast<int>((*widths)[1].asInteger(0));
    const int w2 = static_cast<int>((*widths)[2].asInteger(0));
    if (w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8) {
        if (diagnostic != nullptr) *diagnostic = "xref 串流的 /W 不合理";
        return false;
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    if (const PdfObject* index = stream->dict.find("Index")) {
        if (const PdfArray* array = index->asArray()) {
            for (std::size_t i = 0; i + 1 < array->size(); i += 2) {
                ranges.emplace_back((*array)[i].asInteger(0), (*array)[i + 1].asInteger(0));
            }
        }
    }
    if (ranges.empty()) {
        const PdfObject* size = stream->dict.find("Size");
        ranges.emplace_back(0, size == nullptr ? 0 : size->asInteger(0));
    }

    std::size_t pos = 0;
    for (const auto& range : ranges) {
        for (std::int64_t i = 0; i < range.second; ++i) {
            if (pos >= decoded.data.size()) break;
            // /W 的第一欄可以是 0，代表型別一律視為 1（§7.5.8.2）。
            const std::int64_t type = readWidthField(decoded.data, pos, w0, 1);
            const std::int64_t field2 = readWidthField(decoded.data, pos, w1, 0);
            const std::int64_t field3 = readWidthField(decoded.data, pos, w2, 0);
            const int objectNumber = static_cast<int>(range.first + i);
            if (type == 1) {
                ObjectLocation location{};
                location.offset = static_cast<std::size_t>(field2);
                location.generation = static_cast<int>(field3);
                recordEntry(objectNumber, location);
            } else if (type == 2) {
                ObjectLocation location{};
                location.inObjectStream = true;
                location.containerObject = static_cast<int>(field2);
                location.indexInContainer = static_cast<int>(field3);
                recordEntry(objectNumber, location);
            }
        }
    }

    mergeTrailer(stream->dict);
    if (const PdfObject* prev = stream->dict.find("Prev")) {
        const std::int64_t value = prev->asInteger(-1);
        if (value >= 0 && static_cast<std::size_t>(value) < bytes_.size()) {
            previous = static_cast<std::size_t>(value);
            hasPrevious = true;
        }
    }
    return true;
}

void PdfSourceDocument::mergeTrailer(const PdfDictionary& dict) {
    // 由新到舊走訪，因此先看到的值才是有效值；舊段落的 /Root 可能指向已被
    // 取代的目錄物件，覆蓋過去會讓我們把註解掛到錯的頁面樹上。
    for (const auto& entry : dict.entries()) {
        if (entry.first == "Prev" || entry.first == "XRefStm" || entry.first == "Length" ||
            entry.first == "Filter" || entry.first == "W" || entry.first == "Index" ||
            entry.first == "DecodeParms" || entry.first == "Type") {
            continue;
        }
        if (!trailer_.has(entry.first)) trailer_.set(entry.first, entry.second);
    }
    if (const PdfObject* size = dict.find("Size")) {
        trailerSize_ = std::max(trailerSize_, size->asInteger(0));
    }
}

void PdfSourceDocument::recordEntry(int number, const ObjectLocation& location) {
    if (number <= 0) return;
    // 由新到舊掃描，先記錄的是較新的版本，不可被舊段落蓋掉。
    xref_.emplace(number, location);
}

bool PdfSourceDocument::hasObject(int number) const { return xref_.count(number) != 0; }

int PdfSourceDocument::generationOf(int number) const {
    const auto it = xref_.find(number);
    return it == xref_.end() ? 0 : it->second.generation;
}

std::vector<int> PdfSourceDocument::objectNumbers() const {
    std::vector<int> numbers;
    numbers.reserve(xref_.size());
    for (const auto& entry : xref_) numbers.push_back(entry.first);
    return numbers;
}

PdfObject PdfSourceDocument::object(int number) const {
    if (const auto cached = cache_.find(number); cached != cache_.end()) return cached->second;
    const auto it = xref_.find(number);
    if (it == xref_.end()) return PdfObject{};

    // 迴圈保護：/Length 是間接參照、而該物件又落在同一個物件串流裡時，
    // 天真的實作會無限遞迴。
    if (std::find(loading_.begin(), loading_.end(), number) != loading_.end()) return PdfObject{};
    loading_.push_back(number);
    struct Guard {
        std::vector<int>& stack;
        ~Guard() { stack.pop_back(); }
    } guard{loading_};

    PdfObject result;
    if (!it->second.inObjectStream) {
        if (it->second.offset < bytes_.size()) {
            PdfParser parser(bytes_, it->second.offset);
            parser.setResolver([this](const PdfRef& ref) { return object(ref.number); });
            int parsedNumber = 0;
            int parsedGeneration = 0;
            PdfObject parsed;
            if (parser.parseIndirectObject(parsedNumber, parsedGeneration, parsed) &&
                parsedNumber == number) {
                result = std::move(parsed);
            }
        }
    } else {
        const PdfObject container = object(it->second.containerObject);
        const PdfStream* stream = container.asStream();
        if (stream != nullptr) {
            const DecodeResult decoded =
                decodeStream(*stream, [this](const PdfRef& ref) { return object(ref.number); });
            const PdfObject* countObject = stream->dict.find("N");
            const PdfObject* firstObject = stream->dict.find("First");
            if (decoded.ok && countObject != nullptr && firstObject != nullptr) {
                const std::int64_t count = countObject->asInteger(0);
                const std::int64_t first = firstObject->asInteger(0);
                PdfParser header(decoded.data, 0);
                for (std::int64_t i = 0; i < count; ++i) {
                    std::string numberToken;
                    std::string offsetToken;
                    if (!header.readKeyword(numberToken) || !header.readKeyword(offsetToken)) break;
                    long long entryNumber = 0;
                    long long entryOffset = 0;
                    try {
                        entryNumber = std::stoll(numberToken);
                        entryOffset = std::stoll(offsetToken);
                    } catch (...) {
                        break;
                    }
                    if (static_cast<int>(entryNumber) != number) continue;
                    const std::size_t start = static_cast<std::size_t>(first + entryOffset);
                    if (start >= decoded.data.size()) break;
                    PdfParser body(decoded.data, start);
                    PdfObject parsed;
                    if (body.parseObject(parsed)) result = std::move(parsed);
                    break;
                }
            }
        }
    }

    cache_.emplace(number, result);
    return result;
}

PdfObject PdfSourceDocument::resolve(const PdfObject& value) const {
    if (!value.isRef()) return value;
    return object(value.asRef().number);
}

void PdfSourceDocument::collectPages() {
    const PdfObject* rootRef = trailer_.find("Root");
    if (rootRef == nullptr) return;
    const PdfObject root = resolve(*rootRef);
    const PdfDictionary* catalog = root.asDictionary();
    if (catalog == nullptr) return;
    const PdfObject* pagesRef = catalog->find("Pages");
    if (pagesRef == nullptr || !pagesRef->isRef()) return;

    // 頁面樹以明確的堆疊走訪。惡意檔案可以讓 /Kids 互相指涉，
    // 遞迴版本會直接爆堆疊；visited 同時擋掉重複與環。
    std::vector<PdfRef> stack{pagesRef->asRef()};
    std::set<int> visited;
    int budget = kMaxPageTreeNodes;
    while (!stack.empty() && budget-- > 0) {
        const PdfRef ref = stack.back();
        stack.pop_back();
        if (!visited.insert(ref.number).second) continue;

        const PdfObject node = object(ref.number);
        const PdfDictionary* dict = node.asDictionary();
        if (dict == nullptr) continue;

        const PdfObject* type = dict->find("Type");
        const PdfObject* kids = dict->find("Kids");
        if (kids != nullptr) {
            const PdfObject kidsArray = resolve(*kids);
            if (const PdfArray* array = kidsArray.asArray()) {
                // 反序推入才能讓彈出順序等於頁序。
                for (auto it = array->rbegin(); it != array->rend(); ++it) {
                    if (it->isRef()) stack.push_back(it->asRef());
                }
                continue;
            }
        }
        if (type != nullptr && type->isName("Pages")) continue;
        pages_.push_back(ref);
    }
}

PdfObject PdfSourceDocument::inheritedPageAttribute(const PdfRef& page,
                                                    const std::string& key) const {
    PdfRef current = page;
    std::set<int> visited;
    for (int depth = 0; depth < kMaxInheritDepth; ++depth) {
        if (!visited.insert(current.number).second) break;
        const PdfObject node = object(current.number);
        const PdfDictionary* dict = node.asDictionary();
        if (dict == nullptr) break;
        if (const PdfObject* value = dict->find(key)) return *value;
        const PdfObject* parent = dict->find("Parent");
        if (parent == nullptr || !parent->isRef()) break;
        current = parent->asRef();
    }
    return PdfObject{};
}

}  // namespace alioth::engine::objects
