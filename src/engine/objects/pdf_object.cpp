#include "engine/objects/pdf_object.h"

#include <algorithm>
#include <cmath>

#include "engine/annotations/appearance_stream.h"

namespace alioth::engine::objects {

namespace {

[[nodiscard]] bool isDelimiter(unsigned char c) noexcept {
    switch (c) {
        case '(': case ')': case '<': case '>': case '[': case ']':
        case '{': case '}': case '/': case '%':
            return true;
        default:
            return false;
    }
}

void appendHexByte(std::string& out, unsigned char c) {
    static const char* kDigits = "0123456789ABCDEF";
    out += kDigits[(c >> 4) & 0x0F];
    out += kDigits[c & 0x0F];
}

// UTF-8 → UTF-16BE。自己轉而不借 Qt，是因為這一層要能在無 Qt 的環境下
// 被單元測試；與 annotation_writer 的 UTF-16LE 版本用途不同（那個餵給
// PDFium 的字串 API，這個直接寫進檔案），刻意不共用。
[[nodiscard]] std::string toUtf16Be(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() * 2);
    const auto push = [&out](unsigned int unit) {
        out += static_cast<char>((unit >> 8) & 0xFF);
        out += static_cast<char>(unit & 0xFF);
    };

    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        char32_t code = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code = lead & 0x07u;
            extra = 3;
        } else {
            // 非法前導位元組以替換字元帶過：一個壞掉的作者名不該讓整次存檔失敗。
            code = 0xFFFD;
        }
        if (i + extra >= utf8.size()) {
            code = 0xFFFD;
            extra = 0;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(utf8[i + k]);
            if ((cont & 0xC0) != 0x80) {
                code = 0xFFFD;
                extra = k - 1;
                break;
            }
            code = (code << 6) | (cont & 0x3Fu);
        }
        i += extra + 1;

        if (code >= 0x10000 && code <= 0x10FFFF) {
            code -= 0x10000;
            push(static_cast<unsigned int>(0xD800 + (code >> 10)));
            push(static_cast<unsigned int>(0xDC00 + (code & 0x3FF)));
        } else if (code > 0x10FFFF) {
            push(0xFFFD);
        } else {
            push(static_cast<unsigned int>(code));
        }
    }
    return out;
}

void serializeString(std::string& out, const PdfString& value) {
    if (value.hex) {
        out += '<';
        for (const char c : value.bytes) appendHexByte(out, static_cast<unsigned char>(c));
        out += '>';
        return;
    }
    out += '(';
    out += escapeLiteralString(value.bytes);
    out += ')';
}

}  // namespace

PdfDictionary::PdfDictionary() = default;
PdfDictionary::~PdfDictionary() = default;
PdfDictionary::PdfDictionary(const PdfDictionary&) = default;
PdfDictionary::PdfDictionary(PdfDictionary&&) noexcept = default;
PdfDictionary& PdfDictionary::operator=(const PdfDictionary&) = default;
PdfDictionary& PdfDictionary::operator=(PdfDictionary&&) noexcept = default;

void PdfDictionary::set(std::string key, PdfObject value) {
    for (Entry& entry : entries_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

void PdfDictionary::remove(const std::string& key) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&key](const Entry& e) { return e.first == key; }),
                   entries_.end());
}

bool PdfDictionary::has(const std::string& key) const { return find(key) != nullptr; }

const PdfObject* PdfDictionary::find(const std::string& key) const {
    for (const Entry& entry : entries_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

PdfObject* PdfDictionary::find(const std::string& key) {
    for (Entry& entry : entries_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

PdfObject::PdfObject() : value_(PdfNull{}) {}
PdfObject::~PdfObject() = default;
PdfObject::PdfObject(const PdfObject&) = default;
PdfObject::PdfObject(PdfObject&&) noexcept = default;
PdfObject& PdfObject::operator=(const PdfObject&) = default;
PdfObject& PdfObject::operator=(PdfObject&&) noexcept = default;

PdfObject::PdfObject(PdfNull) : value_(PdfNull{}) {}
PdfObject::PdfObject(bool value) : value_(value) {}
PdfObject::PdfObject(std::int64_t value) : value_(value) {}
PdfObject::PdfObject(int value) : value_(static_cast<std::int64_t>(value)) {}
PdfObject::PdfObject(double value) : value_(value) {}
PdfObject::PdfObject(PdfName value) : value_(std::move(value)) {}
PdfObject::PdfObject(PdfString value) : value_(std::move(value)) {}
PdfObject::PdfObject(PdfArray value) : value_(std::move(value)) {}
PdfObject::PdfObject(PdfDictionary value) : value_(std::move(value)) {}
PdfObject::PdfObject(PdfStream value) : value_(std::move(value)) {}
PdfObject::PdfObject(PdfRef value) : value_(value) {}
PdfObject::PdfObject(PdfRawLiteral value) : value_(std::move(value)) {}

bool PdfObject::isNull() const noexcept { return std::holds_alternative<PdfNull>(value_); }

bool PdfObject::isNumber() const noexcept {
    return std::holds_alternative<std::int64_t>(value_) || std::holds_alternative<double>(value_);
}

bool PdfObject::isName(const char* expected) const noexcept {
    const auto* name = std::get_if<PdfName>(&value_);
    if (name == nullptr) return false;
    return expected == nullptr || name->value == expected;
}

bool PdfObject::isRef() const noexcept { return std::holds_alternative<PdfRef>(value_); }
bool PdfObject::isArray() const noexcept { return std::holds_alternative<PdfArray>(value_); }

bool PdfObject::isDictionary() const noexcept {
    return std::holds_alternative<PdfDictionary>(value_) || isStream();
}

bool PdfObject::isStream() const noexcept { return std::holds_alternative<PdfStream>(value_); }

double PdfObject::asNumber(double fallback) const noexcept {
    if (const auto* i = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*i);
    if (const auto* d = std::get_if<double>(&value_)) return *d;
    return fallback;
}

std::int64_t PdfObject::asInteger(std::int64_t fallback) const noexcept {
    if (const auto* i = std::get_if<std::int64_t>(&value_)) return *i;
    if (const auto* d = std::get_if<double>(&value_)) {
        if (!std::isfinite(*d)) return fallback;
        return static_cast<std::int64_t>(*d);
    }
    return fallback;
}

std::string PdfObject::asName() const {
    if (const auto* name = std::get_if<PdfName>(&value_)) return name->value;
    return {};
}

PdfRef PdfObject::asRef() const noexcept {
    if (const auto* ref = std::get_if<PdfRef>(&value_)) return *ref;
    return PdfRef{};
}

const PdfArray* PdfObject::asArray() const noexcept { return std::get_if<PdfArray>(&value_); }
PdfArray* PdfObject::asArray() noexcept { return std::get_if<PdfArray>(&value_); }

const PdfDictionary* PdfObject::asDictionary() const noexcept {
    if (const auto* dict = std::get_if<PdfDictionary>(&value_)) return dict;
    if (const auto* stream = std::get_if<PdfStream>(&value_)) return &stream->dict;
    return nullptr;
}

PdfDictionary* PdfObject::asDictionary() noexcept {
    if (auto* dict = std::get_if<PdfDictionary>(&value_)) return dict;
    if (auto* stream = std::get_if<PdfStream>(&value_)) return &stream->dict;
    return nullptr;
}

const PdfStream* PdfObject::asStream() const noexcept { return std::get_if<PdfStream>(&value_); }
PdfStream* PdfObject::asStream() noexcept { return std::get_if<PdfStream>(&value_); }

std::string formatReal(double value) { return annotations::formatNumber(value); }

std::string escapeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F || c == '#' || isDelimiter(c)) {
            out += '#';
            appendHexByte(out, c);
        } else {
            out += ch;
        }
    }
    return out;
}

std::string escapeLiteralString(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() + 8);
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\\': out += "\\\\"; continue;
            case '(':  out += "\\(";  continue;
            case ')':  out += "\\)";  continue;
            case '\n': out += "\\n";  continue;
            // CR 一定要跳脫：多數工具在複製檔案時會把裸露的 CR 正規化成 LF，
            // 那會靜默改掉字串內容，而檔案本身仍然合法，錯誤查不出來源。
            case '\r': out += "\\r";  continue;
            case '\t': out += "\\t";  continue;
            case '\b': out += "\\b";  continue;
            case '\f': out += "\\f";  continue;
            default: break;
        }
        if (c < 0x20 || c == 0x7F) {
            out += '\\';
            out += static_cast<char>('0' + ((c >> 6) & 0x07));
            out += static_cast<char>('0' + ((c >> 3) & 0x07));
            out += static_cast<char>('0' + (c & 0x07));
            continue;
        }
        out += ch;
    }
    return out;
}

PdfObject makeTextString(const std::string& utf8) {
    const bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x80;
    });
    if (ascii) return PdfObject{PdfString{utf8, false}};

    std::string bytes;
    bytes += static_cast<char>(0xFE);
    bytes += static_cast<char>(0xFF);
    bytes += toUtf16Be(utf8);
    return PdfObject{PdfString{std::move(bytes), true}};
}

PdfObject makeLiteralString(const std::string& bytes) { return PdfObject{PdfString{bytes, false}}; }

PdfObject makeName(std::string name) { return PdfObject{PdfName{std::move(name)}}; }

PdfObject makeRef(int number, int generation) { return PdfObject{PdfRef{number, generation}}; }

PdfObject makeNumberArray(const std::vector<double>& values) {
    PdfArray array;
    array.reserve(values.size());
    for (const double v : values) array.emplace_back(v);
    return PdfObject{std::move(array)};
}

void serializeInto(std::string& out, const PdfObject& object) {
    struct Visitor {
        std::string& out;

        void operator()(const PdfNull&) const { out += "null"; }
        void operator()(bool value) const { out += value ? "true" : "false"; }
        void operator()(std::int64_t value) const { out += std::to_string(value); }
        void operator()(double value) const { out += formatReal(value); }
        void operator()(const PdfName& value) const {
            out += '/';
            out += escapeName(value.value);
        }
        void operator()(const PdfString& value) const { serializeString(out, value); }
        void operator()(const PdfRef& value) const {
            out += std::to_string(value.number);
            out += ' ';
            out += std::to_string(value.generation);
            out += " R";
        }
        void operator()(const PdfRawLiteral& value) const { out += value.bytes; }
        void operator()(const PdfArray& value) const {
            out += '[';
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i != 0) out += ' ';
                serializeInto(out, value[i]);
            }
            out += ']';
        }
        void operator()(const PdfDictionary& value) const {
            out += "<<";
            for (const auto& entry : value.entries()) {
                out += '/';
                out += escapeName(entry.first);
                // 名稱與值之間需要分隔符，但值若以分隔符開頭（陣列、字典、
                // 名稱、字串）就不必再加空白，省下的位元組數在大量註解時可觀。
                const auto& v = entry.second.value();
                const bool selfDelimited = std::holds_alternative<PdfArray>(v) ||
                                           std::holds_alternative<PdfDictionary>(v) ||
                                           std::holds_alternative<PdfName>(v) ||
                                           std::holds_alternative<PdfString>(v) ||
                                           std::holds_alternative<PdfStream>(v);
                if (!selfDelimited) out += ' ';
                serializeInto(out, entry.second);
            }
            out += ">>";
        }
        void operator()(const PdfStream& value) const {
            // /Length 由實際資料長度決定。讓呼叫端自己維護它是壞掉的檔案最常見的
            // 來源之一，而且錯誤只在某些解析器上顯現。
            PdfDictionary dict = value.dict;
            dict.set("Length", PdfObject{static_cast<std::int64_t>(value.data.size())});
            (*this)(dict);
            out += "\nstream\n";
            out += value.data;
            out += "\nendstream";
        }
    };

    std::visit(Visitor{out}, object.value());
}

std::string serialize(const PdfObject& object) {
    std::string out;
    serializeInto(out, object);
    return out;
}

std::string serializeIndirect(int number, int generation, const PdfObject& object) {
    std::string out = std::to_string(number);
    out += ' ';
    out += std::to_string(generation);
    out += " obj\n";
    serializeInto(out, object);
    out += "\nendobj\n";
    return out;
}

}  // namespace alioth::engine::objects
