#include "engine/objects/pdf_parser.h"

#include <QByteArray>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace alioth::engine::objects {

namespace {

// 巢狀深度上限。PDF 是不可信任輸入，惡意檔案可以用一萬層陣列把堆疊爆掉；
// 真實文件的字典巢狀不會超過個位數。
constexpr int kMaxDepth = 64;

[[nodiscard]] bool isWhitespace(unsigned char c) noexcept {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

[[nodiscard]] bool isDelimiter(unsigned char c) noexcept {
    switch (c) {
        case '(': case ')': case '<': case '>': case '[': case ']':
        case '{': case '}': case '/': case '%':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool isRegular(unsigned char c) noexcept {
    return !isWhitespace(c) && !isDelimiter(c);
}

[[nodiscard]] int hexValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 解析十進位數字。刻意不用 strtod 的地區設定敏感路徑以外的理由：
// PDF 只允許 [+-]?digits[.digits]，容忍更多語法會把損毀的檔案當成正常檔案讀。
[[nodiscard]] bool parseNumberToken(const std::string& token, bool& isInteger, double& real,
                                    std::int64_t& integer) {
    if (token.empty()) return false;
    std::size_t i = 0;
    bool negative = false;
    if (token[i] == '+' || token[i] == '-') {
        negative = token[i] == '-';
        ++i;
    }
    bool sawDigit = false;
    std::int64_t whole = 0;
    while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
        sawDigit = true;
        if (whole < 100000000000000LL) whole = whole * 10 + (token[i] - '0');
        ++i;
    }
    double fraction = 0.0;
    bool hasFraction = false;
    if (i < token.size() && token[i] == '.') {
        hasFraction = true;
        ++i;
        double scale = 0.1;
        while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
            sawDigit = true;
            fraction += static_cast<double>(token[i] - '0') * scale;
            scale *= 0.1;
            ++i;
        }
    }
    if (!sawDigit || i != token.size()) return false;

    isInteger = !hasFraction;
    integer = negative ? -whole : whole;
    const double magnitude = static_cast<double>(whole) + fraction;
    real = negative ? -magnitude : magnitude;
    return true;
}

// PNG 預測器還原（ISO 32000 §7.4.4.4）。xref 串流幾乎一律帶 /Predictor 12，
// 少了這一步解出來的偏移量會是一堆遞增的差值，症狀是所有物件都指到錯的位置。
[[nodiscard]] bool undoPngPredictor(const std::string& input, int colors, int bitsPerComponent,
                                    int columns, std::string& out) {
    if (colors <= 0 || bitsPerComponent <= 0 || columns <= 0) return false;
    const std::size_t bitsPerPixel = static_cast<std::size_t>(colors) *
                                     static_cast<std::size_t>(bitsPerComponent);
    const std::size_t bytesPerPixel = std::max<std::size_t>(1, bitsPerPixel / 8);
    const std::size_t rowLength =
        (static_cast<std::size_t>(columns) * bitsPerPixel + 7) / 8;
    if (rowLength == 0) return false;

    std::vector<unsigned char> previous(rowLength, 0);
    std::vector<unsigned char> current(rowLength, 0);
    out.clear();
    std::size_t pos = 0;
    while (pos + 1 <= input.size()) {
        const auto filter = static_cast<unsigned char>(input[pos]);
        ++pos;
        const std::size_t available = std::min(rowLength, input.size() - pos);
        if (available == 0) break;
        std::fill(current.begin(), current.end(), static_cast<unsigned char>(0));
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), available, current.begin());
        pos += available;

        for (std::size_t i = 0; i < rowLength; ++i) {
            const int left = i >= bytesPerPixel ? current[i - bytesPerPixel] : 0;
            const int up = previous[i];
            const int upLeft = i >= bytesPerPixel ? previous[i - bytesPerPixel] : 0;
            int value = current[i];
            switch (filter) {
                case 0: break;
                case 1: value += left; break;
                case 2: value += up; break;
                case 3: value += (left + up) / 2; break;
                case 4: {
                    const int p = left + up - upLeft;
                    const int pa = std::abs(p - left);
                    const int pb = std::abs(p - up);
                    const int pc = std::abs(p - upLeft);
                    if (pa <= pb && pa <= pc) value += left;
                    else if (pb <= pc) value += up;
                    else value += upLeft;
                    break;
                }
                default:
                    return false;
            }
            current[i] = static_cast<unsigned char>(value & 0xFF);
        }
        out.append(reinterpret_cast<const char*>(current.data()), rowLength);
        previous = current;
    }
    return true;
}

[[nodiscard]] PdfObject resolveIfRef(const PdfObject& object, const ObjectResolver& resolver) {
    if (!object.isRef() || !resolver) return object;
    return resolver(object.asRef());
}

}  // namespace

bool PdfParser::skipWhitespace() {
    while (pos_ < bytes_.size()) {
        const auto c = static_cast<unsigned char>(bytes_[pos_]);
        if (isWhitespace(c)) {
            ++pos_;
            continue;
        }
        if (c == '%') {
            while (pos_ < bytes_.size() && bytes_[pos_] != '\n' && bytes_[pos_] != '\r') ++pos_;
            continue;
        }
        return true;
    }
    return false;
}

bool PdfParser::readKeyword(std::string& out) {
    if (!skipWhitespace()) return false;
    const std::size_t start = pos_;
    while (pos_ < bytes_.size() && isRegular(static_cast<unsigned char>(bytes_[pos_]))) ++pos_;
    if (pos_ == start) return false;
    out.assign(bytes_.substr(start, pos_ - start));
    return true;
}

bool PdfParser::consumeKeyword(std::string_view keyword) {
    const std::size_t saved = pos_;
    std::string token;
    if (readKeyword(token) && token == keyword) return true;
    pos_ = saved;
    return false;
}

bool PdfParser::parseObject(PdfObject& out) { return parseValue(out, 0); }

bool PdfParser::parseIndirectObject(int& number, int& generation, PdfObject& out) {
    std::string token;
    if (!readKeyword(token)) return false;
    bool isInteger = false;
    double real = 0.0;
    std::int64_t integer = 0;
    if (!parseNumberToken(token, isInteger, real, integer) || !isInteger) return false;
    number = static_cast<int>(integer);

    if (!readKeyword(token)) return false;
    if (!parseNumberToken(token, isInteger, real, integer) || !isInteger) return false;
    generation = static_cast<int>(integer);

    if (!consumeKeyword("obj")) return false;
    return parseValue(out, 0);
}

bool PdfParser::parseValue(PdfObject& out, int depth) {
    if (depth > kMaxDepth) return false;
    if (!skipWhitespace()) return false;

    const char c = bytes_[pos_];
    switch (c) {
        case '/': return parseName(out);
        case '(': return parseLiteralString(out);
        case '[': return parseArray(out, depth);
        case '<':
            if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '<') {
                return parseDictionaryOrStream(out, depth);
            }
            return parseHexString(out);
        case ']':
        case '>':
        case ')':
        case '}':
            return false;
        default:
            break;
    }

    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) return parseNumberOrRef(out);

    std::string keyword;
    if (!readKeyword(keyword)) return false;
    if (keyword == "true") {
        out = PdfObject{true};
        return true;
    }
    if (keyword == "false") {
        out = PdfObject{false};
        return true;
    }
    if (keyword == "null") {
        out = PdfObject{PdfNull{}};
        return true;
    }
    return false;
}

bool PdfParser::parseName(PdfObject& out) {
    ++pos_;  // '/'
    std::string name;
    while (pos_ < bytes_.size() && isRegular(static_cast<unsigned char>(bytes_[pos_]))) {
        const char ch = bytes_[pos_];
        if (ch == '#' && pos_ + 2 < bytes_.size()) {
            const int hi = hexValue(static_cast<unsigned char>(bytes_[pos_ + 1]));
            const int lo = hexValue(static_cast<unsigned char>(bytes_[pos_ + 2]));
            if (hi >= 0 && lo >= 0) {
                name += static_cast<char>((hi << 4) | lo);
                pos_ += 3;
                continue;
            }
        }
        name += ch;
        ++pos_;
    }
    out = makeName(std::move(name));
    return true;
}

bool PdfParser::parseLiteralString(PdfObject& out) {
    ++pos_;  // '('
    std::string value;
    int nesting = 1;
    while (pos_ < bytes_.size()) {
        const char ch = bytes_[pos_++];
        if (ch == '\\') {
            if (pos_ >= bytes_.size()) break;
            const char esc = bytes_[pos_++];
            switch (esc) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case '(': value += '('; break;
                case ')': value += ')'; break;
                case '\\': value += '\\'; break;
                case '\r':
                    // 行接續：反斜線後的換行不產生字元。CRLF 要一起吃掉。
                    if (pos_ < bytes_.size() && bytes_[pos_] == '\n') ++pos_;
                    break;
                case '\n': break;
                default:
                    if (esc >= '0' && esc <= '7') {
                        int code = esc - '0';
                        for (int k = 0; k < 2 && pos_ < bytes_.size(); ++k) {
                            const char d = bytes_[pos_];
                            if (d < '0' || d > '7') break;
                            code = code * 8 + (d - '0');
                            ++pos_;
                        }
                        value += static_cast<char>(code & 0xFF);
                    } else {
                        value += esc;
                    }
                    break;
            }
            continue;
        }
        if (ch == '(') {
            ++nesting;
            value += ch;
            continue;
        }
        if (ch == ')') {
            if (--nesting == 0) {
                out = PdfObject{PdfString{std::move(value), false}};
                return true;
            }
            value += ch;
            continue;
        }
        value += ch;
    }
    return false;
}

bool PdfParser::parseHexString(PdfObject& out) {
    ++pos_;  // '<'
    std::string value;
    int high = -1;
    while (pos_ < bytes_.size()) {
        const char ch = bytes_[pos_++];
        if (ch == '>') {
            // 奇數個十六進位字元時最後一個補 0（§7.3.4.3）。
            if (high >= 0) value += static_cast<char>(high << 4);
            out = PdfObject{PdfString{std::move(value), true}};
            return true;
        }
        const int digit = hexValue(static_cast<unsigned char>(ch));
        if (digit < 0) continue;  // 空白可以出現在十六進位字串中間
        if (high < 0) {
            high = digit;
        } else {
            value += static_cast<char>((high << 4) | digit);
            high = -1;
        }
    }
    return false;
}

bool PdfParser::parseNumberOrRef(PdfObject& out) {
    const std::size_t saved = pos_;
    std::string token;
    if (!readKeyword(token)) return false;
    bool isInteger = false;
    double real = 0.0;
    std::int64_t integer = 0;
    if (!parseNumberToken(token, isInteger, real, integer)) {
        pos_ = saved;
        return false;
    }

    if (isInteger && integer > 0) {
        // 「N G R」與「N」「G」兩個數字在語法上要靠往前看三個 token 才分得出來。
        const std::size_t afterFirst = pos_;
        std::string second;
        if (readKeyword(second)) {
            bool secondIsInteger = false;
            double secondReal = 0.0;
            std::int64_t secondInteger = 0;
            if (parseNumberToken(second, secondIsInteger, secondReal, secondInteger) &&
                secondIsInteger && secondInteger >= 0 && consumeKeyword("R")) {
                out = makeRef(static_cast<int>(integer), static_cast<int>(secondInteger));
                return true;
            }
        }
        pos_ = afterFirst;
    }

    out = isInteger ? PdfObject{integer} : PdfObject{real};
    return true;
}

bool PdfParser::parseArray(PdfObject& out, int depth) {
    ++pos_;  // '['
    PdfArray array;
    while (true) {
        if (!skipWhitespace()) return false;
        if (bytes_[pos_] == ']') {
            ++pos_;
            out = PdfObject{std::move(array)};
            return true;
        }
        PdfObject element;
        if (!parseValue(element, depth + 1)) return false;
        array.push_back(std::move(element));
    }
}

bool PdfParser::parseDictionaryOrStream(PdfObject& out, int depth) {
    pos_ += 2;  // '<<'
    PdfDictionary dict;
    while (true) {
        if (!skipWhitespace()) return false;
        if (bytes_[pos_] == '>') {
            if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '>') {
                pos_ += 2;
                break;
            }
            return false;
        }
        if (bytes_[pos_] != '/') return false;
        PdfObject key;
        if (!parseName(key)) return false;
        PdfObject value;
        if (!parseValue(value, depth + 1)) return false;
        dict.set(key.asName(), std::move(value));
    }

    const std::size_t afterDict = pos_;
    if (consumeKeyword("stream")) {
        std::string data;
        if (!readStreamData(dict, data)) return false;
        out = PdfObject{PdfStream{std::move(dict), std::move(data)}};
        return true;
    }
    pos_ = afterDict;
    out = PdfObject{std::move(dict)};
    return true;
}

bool PdfParser::readStreamData(const PdfDictionary& dict, std::string& out) {
    // 「stream」之後必須恰好接 CRLF 或 LF；單獨的 CR 不合法但實務上存在。
    if (pos_ < bytes_.size() && bytes_[pos_] == '\r') ++pos_;
    if (pos_ < bytes_.size() && bytes_[pos_] == '\n') ++pos_;
    const std::size_t start = pos_;

    std::int64_t length = -1;
    if (const PdfObject* lengthObject = dict.find("Length")) {
        const PdfObject resolved = resolveIfRef(*lengthObject, resolver_);
        if (resolved.isNumber()) length = resolved.asInteger(-1);
    }

    // /Length 可能是壞的（產生檔案的工具寫錯，或它是還沒解析到的間接參照）。
    // 先用它，但一定要驗證後面真的接著 endstream，否則退回搜尋。
    if (length >= 0 && start + static_cast<std::size_t>(length) <= bytes_.size()) {
        std::size_t check = start + static_cast<std::size_t>(length);
        while (check < bytes_.size() && isWhitespace(static_cast<unsigned char>(bytes_[check]))) {
            ++check;
        }
        if (bytes_.compare(check, 9, "endstream") == 0) {
            out.assign(bytes_.substr(start, static_cast<std::size_t>(length)));
            pos_ = check + 9;
            return true;
        }
    }

    const std::size_t found = bytes_.find("endstream", start);
    if (found == std::string_view::npos) return false;
    std::size_t end = found;
    if (end > start && bytes_[end - 1] == '\n') --end;
    if (end > start && bytes_[end - 1] == '\r') --end;
    out.assign(bytes_.substr(start, end - start));
    pos_ = found + 9;
    return true;
}

DecodeResult flateDecode(const std::string& input) {
    DecodeResult result{};
    if (input.empty()) {
        result.ok = true;
        return result;
    }

    // qUncompress 要求前置四個位元組的原始長度。zlib 在緩衝區不足時會回報
    // Z_BUF_ERROR，Qt 會自行加倍重試，但不同 Qt 版本的重試上限不同，
    // 因此這裡由外層再給幾個遞增的猜測值，避免依賴那個實作細節。
    const std::size_t hints[] = {input.size() * 8 + 1024, input.size() * 64 + 65536,
                                 input.size() * 512 + 1048576};
    for (const std::size_t hint : hints) {
        const auto capped = static_cast<quint32>(std::min<std::size_t>(hint, 256u * 1024u * 1024u));
        QByteArray framed;
        framed.reserve(static_cast<qsizetype>(input.size()) + 4);
        framed.append(static_cast<char>((capped >> 24) & 0xFF));
        framed.append(static_cast<char>((capped >> 16) & 0xFF));
        framed.append(static_cast<char>((capped >> 8) & 0xFF));
        framed.append(static_cast<char>(capped & 0xFF));
        framed.append(input.data(), static_cast<qsizetype>(input.size()));

        const QByteArray inflated =
            qUncompress(reinterpret_cast<const uchar*>(framed.constData()), framed.size());
        if (!inflated.isEmpty()) {
            result.ok = true;
            result.data.assign(inflated.constData(), static_cast<std::size_t>(inflated.size()));
            return result;
        }
    }

    result.diagnostic = "FlateDecode 解壓失敗";
    return result;
}

DecodeResult decodeStream(const PdfStream& stream, const ObjectResolver& resolver) {
    DecodeResult result{};

    std::vector<std::string> filters;
    if (const PdfObject* filter = stream.dict.find("Filter")) {
        const PdfObject resolved = resolveIfRef(*filter, resolver);
        if (resolved.isName()) {
            filters.push_back(resolved.asName());
        } else if (const PdfArray* array = resolved.asArray()) {
            for (const PdfObject& entry : *array) filters.push_back(entry.asName());
        }
    }

    std::vector<const PdfDictionary*> parms;
    PdfObject resolvedParms;
    if (const PdfObject* p = stream.dict.find("DecodeParms")) {
        resolvedParms = resolveIfRef(*p, resolver);
        if (const PdfDictionary* dict = resolvedParms.asDictionary()) {
            parms.push_back(dict);
        } else if (const PdfArray* array = resolvedParms.asArray()) {
            for (const PdfObject& entry : *array) parms.push_back(entry.asDictionary());
        }
    }

    std::string data = stream.data;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        const std::string& name = filters[i];
        if (name.empty()) continue;
        if (name != "FlateDecode" && name != "Fl") {
            result.diagnostic = "不支援的串流濾鏡：" + name;
            return result;
        }
        const DecodeResult inflated = flateDecode(data);
        if (!inflated.ok) return inflated;
        data = inflated.data;

        const PdfDictionary* parm = i < parms.size() ? parms[i] : nullptr;
        if (parm == nullptr) continue;
        const PdfObject* predictorObject = parm->find("Predictor");
        const int predictor =
            predictorObject == nullptr ? 1 : static_cast<int>(predictorObject->asInteger(1));
        if (predictor <= 1) continue;
        if (predictor < 10) {
            result.diagnostic = "不支援的預測器：" + std::to_string(predictor);
            return result;
        }
        const auto readInt = [parm](const char* key, int fallback) {
            const PdfObject* value = parm->find(key);
            return value == nullptr ? fallback : static_cast<int>(value->asInteger(fallback));
        };
        std::string undone;
        if (!undoPngPredictor(data, readInt("Colors", 1), readInt("BitsPerComponent", 8),
                              readInt("Columns", 1), undone)) {
            result.diagnostic = "PNG 預測器還原失敗";
            return result;
        }
        data = std::move(undone);
    }

    result.ok = true;
    result.data = std::move(data);
    return result;
}

}  // namespace alioth::engine::objects
