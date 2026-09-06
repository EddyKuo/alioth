#include "engine/redaction/content_redactor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>

#include "engine/objects/pdf_parser.h"
#include "engine/redaction/content_font_metrics.h"

namespace alioth::engine::redaction {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;

// Form XObject 可以互相巢狀。深度上限是防惡意輸入，不是效能考量。
constexpr int kMaxFormDepth = 12;

// 顯示字串沒有精確的垂直範圍可用時的估計值（em）。刻意比多數字型的實際
// 上下界寬：估窄會讓貼著塗黑區域上下緣的那一行被判定為區域外而留在檔案裡。
constexpr double kFallbackAscent = 1.0;
constexpr double kFallbackDescent = -0.35;

bool isWhitespaceByte(unsigned char c) {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

bool isDelimiterByte(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '/' || c == '%';
}

enum class TokenKind {
    Number,
    String,
    Name,
    ArrayStart,
    ArrayEnd,
    DictStart,
    DictEnd,
    Operator,
    End,
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::size_t begin{0};
    std::size_t end{0};
    double number{0.0};
    std::string text{};  // 字串的已解碼位元組、名稱、或運算子
};

// 內容串流的語彙分析。
//
// 刻意不重用 PdfParser：那一層認得間接參照（「1 0 R」），而內容串流裡沒有
// 參照，卻有大量「數字 數字 運算子」的序列。更關鍵的是這裡需要每個 token 的
// 原始位元組範圍——編輯是把某一段原文換掉，其餘位元組原樣搬運，
// 重新序列化整份內容會改動我們根本沒打算碰的東西。
class ContentLexer {
public:
    explicit ContentLexer(const std::string& bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    void seek(std::size_t position) noexcept { pos_ = position; }

    [[nodiscard]] Token next() {
        skipTrivia();
        Token token;
        token.begin = pos_;
        if (pos_ >= bytes_.size()) {
            token.end = pos_;
            return token;
        }

        const char c = bytes_[pos_];
        switch (c) {
            case '[': ++pos_; token.kind = TokenKind::ArrayStart; token.end = pos_; return token;
            case ']': ++pos_; token.kind = TokenKind::ArrayEnd;   token.end = pos_; return token;
            case '(': return literalString(token);
            case '/': return name(token);
            case '<':
                if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '<') {
                    pos_ += 2;
                    token.kind = TokenKind::DictStart;
                    token.end = pos_;
                    return token;
                }
                return hexString(token);
            case '>':
                if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '>') {
                    pos_ += 2;
                    token.kind = TokenKind::DictEnd;
                    token.end = pos_;
                    return token;
                }
                ++pos_;
                token.kind = TokenKind::Operator;
                token.end = pos_;
                return token;
            default: break;
        }

        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') return number(token);
        return oper(token);
    }

private:
    void skipTrivia() {
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c)) {
                ++pos_;
                continue;
            }
            if (c == '%') {
                while (pos_ < bytes_.size() && bytes_[pos_] != '\n' && bytes_[pos_] != '\r') ++pos_;
                continue;
            }
            return;
        }
    }

    Token literalString(Token token) {
        token.kind = TokenKind::String;
        ++pos_;  // '('
        int depth = 1;
        std::string out;
        while (pos_ < bytes_.size() && depth > 0) {
            const char c = bytes_[pos_++];
            if (c == '\\') {
                if (pos_ >= bytes_.size()) break;
                const char escaped = bytes_[pos_++];
                switch (escaped) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '\r':
                        if (pos_ < bytes_.size() && bytes_[pos_] == '\n') ++pos_;
                        break;  // 續行，不產生字元
                    case '\n': break;
                    default:
                        if (escaped >= '0' && escaped <= '7') {
                            int value = escaped - '0';
                            for (int i = 0; i < 2 && pos_ < bytes_.size(); ++i) {
                                const char digit = bytes_[pos_];
                                if (digit < '0' || digit > '7') break;
                                value = value * 8 + (digit - '0');
                                ++pos_;
                            }
                            out += static_cast<char>(value & 0xFF);
                        } else {
                            out += escaped;
                        }
                        break;
                }
                continue;
            }
            if (c == '(') {
                ++depth;
                out += c;
                continue;
            }
            if (c == ')') {
                if (--depth == 0) break;
                out += c;
                continue;
            }
            out += c;
        }
        token.text = std::move(out);
        token.end = pos_;
        return token;
    }

    Token hexString(Token token) {
        token.kind = TokenKind::String;
        ++pos_;  // '<'
        std::string out;
        int high = -1;
        while (pos_ < bytes_.size() && bytes_[pos_] != '>') {
            const char c = bytes_[pos_++];
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else continue;
            if (high < 0) {
                high = digit;
            } else {
                out += static_cast<char>((high << 4) | digit);
                high = -1;
            }
        }
        // 奇數個十六進位字元時最後一個補 0，這是 §7.3.4.3 的規定。
        if (high >= 0) out += static_cast<char>(high << 4);
        if (pos_ < bytes_.size()) ++pos_;  // '>'
        token.text = std::move(out);
        token.end = pos_;
        return token;
    }

    Token name(Token token) {
        token.kind = TokenKind::Name;
        ++pos_;  // '/'
        std::string out;
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c) || isDelimiterByte(c)) break;
            if (c == '#' && pos_ + 2 < bytes_.size()) {
                const auto hex = bytes_.substr(pos_ + 1, 2);
                char* stop = nullptr;
                const long value = std::strtol(hex.c_str(), &stop, 16);
                if (stop == hex.c_str() + 2) {
                    out += static_cast<char>(value);
                    pos_ += 3;
                    continue;
                }
            }
            out += static_cast<char>(c);
            ++pos_;
        }
        token.text = std::move(out);
        token.end = pos_;
        return token;
    }

    Token number(Token token) {
        token.kind = TokenKind::Number;
        const std::size_t start = pos_;
        while (pos_ < bytes_.size()) {
            const char c = bytes_[pos_];
            if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
                ++pos_;
                continue;
            }
            break;
        }
        const std::string text = bytes_.substr(start, pos_ - start);
        // strtod 受 LC_NUMERIC 影響，但內容串流一律用小數點；專案其他地方
        // 也已經避開 printf 系列。這裡自行解析以免地區設定改變結果。
        token.number = parseReal(text);
        token.end = pos_;
        return token;
    }

    Token oper(Token token) {
        token.kind = TokenKind::Operator;
        const std::size_t start = pos_;
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c) || isDelimiterByte(c)) break;
            ++pos_;
        }
        if (pos_ == start) ++pos_;  // 無法辨識的單一位元組，前進以免死迴圈
        token.text = bytes_.substr(start, pos_ - start);
        token.end = pos_;
        return token;
    }

    [[nodiscard]] static double parseReal(const std::string& text) {
        bool negative = false;
        std::size_t i = 0;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative = text[i] == '-';
            ++i;
        }
        double integer = 0.0;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            integer = integer * 10.0 + (text[i] - '0');
        }
        double fraction = 0.0;
        if (i < text.size() && text[i] == '.') {
            ++i;
            double scale = 0.1;
            for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                fraction += (text[i] - '0') * scale;
                scale *= 0.1;
            }
        }
        const double value = integer + fraction;
        return negative ? -value : value;
    }

    const std::string& bytes_;
    std::size_t pos_{0};
};

struct TextState {
    Matrix textMatrix{identityMatrix()};
    Matrix lineMatrix{identityMatrix()};
};

struct GraphicsState {
    Matrix ctm{identityMatrix()};
    std::string fontName{};
    double fontSize{0.0};
    double charSpacing{0.0};
    double wordSpacing{0.0};
    double horizontalScale{1.0};  // Tz / 100
    double leading{0.0};
    double rise{0.0};
};

// PDF 實數輸出。與 objects::formatReal 同一份規則，避免兩份真相。
std::string formatReal(double value) { return objects::formatReal(value); }

class StreamRedactor {
public:
    StreamRedactor(PdfDocumentRewriter& document, const ContentRedactionRequest& request, int depth)
        : document_(document), request_(request), depth_(depth) {}

    ContentRedactionResult run(const std::string& content, const PdfObject& resources,
                               const Matrix& baseCtm) {
        resources_ = document_.resolve(resources);
        state_.ctm = baseCtm;

        ContentLexer lexer(content);
        std::vector<Token> operands;
        std::size_t copied = 0;

        while (true) {
            Token token = lexer.next();
            if (token.kind == TokenKind::End) break;
            if (token.kind != TokenKind::Operator) {
                operands.push_back(std::move(token));
                continue;
            }

            const std::size_t operandStart = operands.empty() ? token.begin : operands.front().begin;
            std::optional<std::string> replacement;

            if (token.text == "BI") {
                // 內嵌影像的二進位資料會讓語彙分析錯亂，必須整段跳過而不是逐 token 讀。
                const std::size_t endOfImage = skipInlineImage(content, token.end);
                if (shouldRemoveUnitSquare()) {
                    replacement = std::string{};
                    ++result_.stats.removedInlineImages;
                }
                if (replacement.has_value()) {
                    result_.content.append(content, copied, token.begin - copied);
                    copied = endOfImage;
                }
                lexer.seek(endOfImage);
                operands.clear();
                continue;
            }

            replacement = handleOperator(token, operands);
            if (replacement.has_value()) {
                result_.content.append(content, copied, operandStart - copied);
                result_.content += *replacement;
                copied = token.end;
            }
            operands.clear();
            if (!result_.diagnostic.empty()) {
                result_.ok = false;
                return std::move(result_);
            }
        }

        result_.content.append(content, copied, std::string::npos);
        result_.ok = true;
        return std::move(result_);
    }

private:
    // 目前狀態下，單位正方形（內嵌影像的座標系）是否落在塗黑區域內。
    [[nodiscard]] bool shouldRemoveUnitSquare() const {
        const domain::RectF box = transformedBounds(state_.ctm, domain::RectF{0, 0, 1, 1});
        return domain::shouldRemove(request_.areas, box, request_.imagePolicy);
    }

    [[nodiscard]] std::optional<std::string> handleOperator(const Token& op,
                                                            std::vector<Token>& operands) {
        const std::string& name = op.text;

        if (name == "q") {
            stack_.push_back(state_);
            return std::nullopt;
        }
        if (name == "Q") {
            if (!stack_.empty()) {
                state_ = stack_.back();
                stack_.pop_back();
            }
            return std::nullopt;
        }
        if (name == "cm" && operands.size() >= 6) {
            state_.ctm = multiply(matrixFrom(operands, operands.size() - 6), state_.ctm);
            return std::nullopt;
        }
        if (name == "BT") {
            text_ = TextState{};
            return std::nullopt;
        }
        if (name == "Tf" && operands.size() >= 2) {
            state_.fontName = operands[operands.size() - 2].text;
            state_.fontSize = operands.back().number;
            return std::nullopt;
        }
        if (name == "Tc" && !operands.empty()) {
            state_.charSpacing = operands.back().number;
            return std::nullopt;
        }
        if (name == "Tw" && !operands.empty()) {
            state_.wordSpacing = operands.back().number;
            return std::nullopt;
        }
        if (name == "Tz" && !operands.empty()) {
            state_.horizontalScale = operands.back().number / 100.0;
            return std::nullopt;
        }
        if (name == "TL" && !operands.empty()) {
            state_.leading = operands.back().number;
            return std::nullopt;
        }
        if (name == "Ts" && !operands.empty()) {
            state_.rise = operands.back().number;
            return std::nullopt;
        }
        if (name == "Td" && operands.size() >= 2) {
            translateLine(operands[operands.size() - 2].number, operands.back().number);
            return std::nullopt;
        }
        if (name == "TD" && operands.size() >= 2) {
            state_.leading = -operands.back().number;
            translateLine(operands[operands.size() - 2].number, operands.back().number);
            return std::nullopt;
        }
        if (name == "Tm" && operands.size() >= 6) {
            text_.textMatrix = matrixFrom(operands, operands.size() - 6);
            text_.lineMatrix = text_.textMatrix;
            return std::nullopt;
        }
        if (name == "T*") {
            translateLine(0.0, -state_.leading);
            return std::nullopt;
        }
        if (name == "Tj" && !operands.empty() && operands.back().kind == TokenKind::String) {
            return showSingle(operands.back().text, "");
        }
        if (name == "'" && !operands.empty() && operands.back().kind == TokenKind::String) {
            translateLine(0.0, -state_.leading);
            return showSingle(operands.back().text, "T*\n");
        }
        if (name == "\"" && operands.size() >= 3 && operands.back().kind == TokenKind::String) {
            state_.wordSpacing = operands[operands.size() - 3].number;
            state_.charSpacing = operands[operands.size() - 2].number;
            translateLine(0.0, -state_.leading);
            // 取代時必須把 aw / ac 的設定效果保留下來：它們是圖形狀態的一部分，
            // 後續的顯示運算子仍然看得到。
            std::string prefix = formatReal(state_.wordSpacing) + " Tw " +
                                 formatReal(state_.charSpacing) + " Tc T*\n";
            return showSingle(operands.back().text, prefix);
        }
        if (name == "TJ" && !operands.empty()) {
            return showArray(operands);
        }
        if (name == "Do" && !operands.empty() && operands.back().kind == TokenKind::Name) {
            return handleXObject(operands.back().text);
        }
        return std::nullopt;
    }

    void translateLine(double tx, double ty) {
        text_.lineMatrix = multiply(Matrix{1, 0, 0, 1, tx, ty}, text_.lineMatrix);
        text_.textMatrix = text_.lineMatrix;
    }

    [[nodiscard]] static Matrix matrixFrom(const std::vector<Token>& operands, std::size_t first) {
        return Matrix{operands[first].number,     operands[first + 1].number,
                      operands[first + 2].number, operands[first + 3].number,
                      operands[first + 4].number, operands[first + 5].number};
    }

    [[nodiscard]] const FontMetrics& currentMetrics() {
        const auto cached = metricsCache_.find(state_.fontName);
        if (cached != metricsCache_.end()) return cached->second;

        FontMetrics metrics;
        const PdfDictionary* resources = resources_.asDictionary();
        if (resources != nullptr) {
            if (const PdfObject* fonts = resources->find("Font")) {
                const PdfObject fontDict = document_.resolve(*fonts);
                if (const PdfDictionary* dict = fontDict.asDictionary()) {
                    if (const PdfObject* font = dict->find(state_.fontName)) {
                        metrics = FontMetrics::fromFontDictionary(
                            *font, [this](const PdfObject& value) {
                                return document_.resolve(value);
                            });
                    }
                }
            }
        }
        return metricsCache_.emplace(state_.fontName, std::move(metrics)).first->second;
    }

    // 一段顯示字串在未縮放文字空間的水平位移。
    [[nodiscard]] double advanceOf(const std::string& bytes, const FontMetrics& metrics) const {
        double total = 0.0;
        const std::vector<std::uint32_t> codes = metrics.decode(bytes);
        for (const std::uint32_t code : codes) {
            double advance = metrics.width(code) * state_.fontSize + state_.charSpacing;
            // 字距只套用在單位元組的字碼 32 上。對兩位元組編碼一律不套用，
            // 這是 §9.3.3 的規定，弄反會讓含空白的行寬度整段偏移。
            if (!metrics.isTwoByte() && code == 32) advance += state_.wordSpacing;
            total += advance;
        }
        return total * state_.horizontalScale;
    }

    // 顯示字串的頁面座標外框。
    [[nodiscard]] domain::RectF boundsOf(double advance, const FontMetrics& metrics) const {
        // 字型大小為 0 的內容串流存在（多半是壞掉或刻意隱藏的文字層）。
        // 這時仍要給一個有高度的外框，否則零高度矩形永遠判定為不相交，
        // 那些字就會原封不動留在檔案裡。
        const double size = state_.fontSize != 0.0 ? std::abs(state_.fontSize) : 1.0;
        const double ascent = metrics.ascent() > 0.0 ? metrics.ascent() : kFallbackAscent;
        const double descent = metrics.descent() < 0.0 ? metrics.descent() : kFallbackDescent;

        const double left = std::min(0.0, advance);
        const double right = std::max(0.0, advance);
        const double bottom = state_.rise + descent * size;
        const double top = state_.rise + ascent * size;

        const Matrix toPage = multiply(text_.textMatrix, state_.ctm);
        return transformedBounds(toPage, domain::RectF{left, bottom, right, top});
    }

    // 移除一段字串後補回等寬位移的 TJ 調整量。
    //
    // TJ 的數字單位是 1/1000 文字空間，實際位移是 -adj/1000 × Tfs × Th，
    // 因此要抵銷 advance 就取 adj = -advance × 1000 /(Tfs × Th)。
    [[nodiscard]] std::string displacementFor(double advance) const {
        const double denominator = state_.fontSize * state_.horizontalScale;
        if (std::abs(denominator) < 1e-9 || std::abs(advance) < 1e-9) return {};
        const double adjustment = -advance * 1000.0 / denominator;
        return "[" + formatReal(adjustment) + "]TJ";
    }

    [[nodiscard]] std::optional<std::string> showSingle(const std::string& bytes,
                                                        const std::string& prefix) {
        const FontMetrics& metrics = currentMetrics();
        const double advance = advanceOf(bytes, metrics);
        const domain::RectF box = boundsOf(advance, metrics);
        const bool remove = domain::shouldRemove(request_.areas, box, request_.textPolicy);

        text_.textMatrix = multiply(Matrix{1, 0, 0, 1, advance, 0}, text_.textMatrix);
        if (!remove) return std::nullopt;

        ++result_.stats.removedStrings;
        return prefix + displacementFor(advance);
    }

    // TJ 的每個字串元素各自是一段顯示字串，因此判定與移除都以元素為單位。
    // 這比「整個 TJ 一起移除」精確，卻仍然不會切開任何一段字碼——
    // 切一半留下的殘字對法務用途等同外洩。
    [[nodiscard]] std::optional<std::string> showArray(const std::vector<Token>& operands) {
        std::size_t open = operands.size();
        for (std::size_t i = operands.size(); i-- > 0;) {
            if (operands[i].kind == TokenKind::ArrayStart) {
                open = i;
                break;
            }
        }
        if (open == operands.size()) return std::nullopt;

        const FontMetrics& metrics = currentMetrics();
        std::string rebuilt = "[";
        bool removedAny = false;
        for (std::size_t i = open + 1; i < operands.size(); ++i) {
            const Token& item = operands[i];
            if (item.kind == TokenKind::ArrayEnd) break;
            if (item.kind == TokenKind::Number) {
                const double displacement =
                    -item.number / 1000.0 * state_.fontSize * state_.horizontalScale;
                text_.textMatrix = multiply(Matrix{1, 0, 0, 1, displacement, 0}, text_.textMatrix);
                rebuilt += formatReal(item.number);
                rebuilt += ' ';
                continue;
            }
            if (item.kind != TokenKind::String) continue;

            const double advance = advanceOf(item.text, metrics);
            const domain::RectF box = boundsOf(advance, metrics);
            const bool remove = domain::shouldRemove(request_.areas, box, request_.textPolicy);
            text_.textMatrix = multiply(Matrix{1, 0, 0, 1, advance, 0}, text_.textMatrix);

            if (remove) {
                removedAny = true;
                ++result_.stats.removedStrings;
                const double denominator = state_.fontSize * state_.horizontalScale;
                if (std::abs(denominator) > 1e-9 && std::abs(advance) > 1e-9) {
                    rebuilt += formatReal(-advance * 1000.0 / denominator);
                    rebuilt += ' ';
                }
                continue;
            }
            rebuilt += objects::serialize(objects::makeLiteralString(item.text));
            rebuilt += ' ';
        }
        rebuilt += "]TJ";
        if (!removedAny) return std::nullopt;
        return rebuilt;
    }

    [[nodiscard]] std::optional<std::string> handleXObject(const std::string& name) {
        const PdfDictionary* resources = resources_.asDictionary();
        if (resources == nullptr) return std::nullopt;
        const PdfObject* xobjects = resources->find("XObject");
        if (xobjects == nullptr) return std::nullopt;
        const PdfObject group = document_.resolve(*xobjects);
        const PdfDictionary* groupDict = group.asDictionary();
        if (groupDict == nullptr) return std::nullopt;
        const PdfObject* entry = groupDict->find(name);
        if (entry == nullptr || !entry->isRef()) return std::nullopt;

        const int number = entry->asRef().number;
        const PdfObject* target = document_.object(number);
        if (target == nullptr) return std::nullopt;
        const PdfDictionary* dict = target->asDictionary();
        if (dict == nullptr) return std::nullopt;
        const PdfObject* subtype = dict->find("Subtype");

        if (subtype != nullptr && subtype->isName("Image")) {
            const domain::RectF box = transformedBounds(state_.ctm, domain::RectF{0, 0, 1, 1});
            if (!domain::shouldRemove(request_.areas, box, request_.imagePolicy)) {
                return std::nullopt;
            }
            ++result_.stats.removedImages;
            // 名稱回報給呼叫端從資源字典移除。少了那一步，影像物件仍然可達，
            // 它的像素資料會原封不動留在輸出檔案裡——黑框底下的照片還在。
            result_.removedXObjectNames.push_back(name);
            return std::string{};
        }

        if (subtype != nullptr && subtype->isName("Form")) {
            redactForm(number);
            return std::nullopt;
        }
        return std::nullopt;
    }

    void redactForm(int number) {
        if (depth_ >= kMaxFormDepth) return;

        const PdfObject* target = document_.object(number);
        const objects::PdfStream* stream = target == nullptr ? nullptr : target->asStream();
        if (stream == nullptr) return;

        Matrix formMatrix = identityMatrix();
        if (const PdfObject* matrix = stream->dict.find("Matrix")) {
            const PdfObject resolved = document_.resolve(*matrix);
            if (const PdfArray* array = resolved.asArray(); array != nullptr && array->size() >= 6) {
                for (std::size_t i = 0; i < 6; ++i) {
                    formMatrix[i] = (*array)[i].asNumber(formMatrix[i]);
                }
            }
        }
        const Matrix formCtm = multiply(formMatrix, state_.ctm);

        domain::RectF bbox{0, 0, 0, 0};
        bool hasBBox = false;
        if (const PdfObject* box = stream->dict.find("BBox")) {
            const PdfObject resolved = document_.resolve(*box);
            if (const PdfArray* array = resolved.asArray(); array != nullptr && array->size() >= 4) {
                bbox = domain::RectF{(*array)[0].asNumber(0.0), (*array)[1].asNumber(0.0),
                                     (*array)[2].asNumber(0.0), (*array)[3].asNumber(0.0)}
                           .normalized();
                hasBBox = true;
            }
        }
        // /BBox 是必填欄位；缺了它就無法判斷這個表單是否與塗黑區域相交，
        // 此時一律遞迴進去，而不是假設它在區域外。
        if (hasBBox && !domain::overlapsAny(request_.areas, transformedBounds(formCtm, bbox))) {
            return;
        }

        if (document_.referenceCount(number) > 1) {
            // 被多處引用的 Form XObject 就地改寫會連帶改到沒有要求塗黑的頁面。
            // 這裡明確失敗而不是二選一：靜默略過會外洩，靜默改寫會毀掉別頁。
            result_.diagnostic =
                "Form XObject " + std::to_string(number) +
                " 與塗黑區域相交，但它被多處共用，無法在不影響其他頁面的前提下改寫";
            return;
        }

        const objects::DecodeResult decoded =
            objects::decodeStream(*stream, [this](const objects::PdfRef& ref) {
                const PdfObject* found = document_.object(ref.number);
                return found == nullptr ? PdfObject{} : *found;
            });
        if (!decoded.ok) {
            result_.diagnostic = "Form XObject " + std::to_string(number) +
                                 " 的濾鏡無法解開，因此無法確認其中是否有要移除的內容：" +
                                 decoded.diagnostic;
            return;
        }

        PdfObject resources;
        if (const PdfObject* own = stream->dict.find("Resources")) {
            resources = *own;
        } else {
            resources = resources_;  // 表單沒有自己的資源時沿用外層的
        }

        const ContentRedactionResult nested = redactContentStream(
            document_, decoded.data, resources, formCtm, request_, depth_ + 1);
        if (!nested.ok) {
            result_.diagnostic = nested.diagnostic;
            return;
        }

        PdfObject* mutableTarget = document_.object(number);
        objects::PdfStream* mutableStream = mutableTarget->asStream();
        mutableStream->data = nested.content;
        // 編輯後的位元組是未壓縮的，濾鏡宣告必須跟著拿掉，否則解析器會把
        // 明文當成 Flate 資料而整段解析失敗。
        mutableStream->dict.remove("Filter");
        mutableStream->dict.remove("DecodeParms");

        if (!nested.removedXObjectNames.empty()) {
            (void)removeResourceEntries(document_, number, "XObject", nested.removedXObjectNames);
        }
        result_.stats.removedStrings += nested.stats.removedStrings;
        result_.stats.removedImages += nested.stats.removedImages;
        result_.stats.removedInlineImages += nested.stats.removedInlineImages;
        result_.stats.editedForms += nested.stats.editedForms + 1;
    }

    // BI …（字典）… ID <二進位> EI。二進位資料裡可能出現任何位元組，
    // 因此結束標記必須以「前後都是分隔符的 EI」判定，不能直接 find("EI")。
    [[nodiscard]] static std::size_t skipInlineImage(const std::string& content,
                                                     std::size_t afterBI) {
        std::size_t pos = afterBI;
        // 先找 ID：字典部分是一般 token，可以逐位元組掃。
        while (pos + 1 < content.size()) {
            if (content[pos] == 'I' && content[pos + 1] == 'D') {
                pos += 2;
                break;
            }
            ++pos;
        }
        if (pos < content.size() && isWhitespaceByte(static_cast<unsigned char>(content[pos]))) {
            ++pos;
        }
        while (pos + 1 < content.size()) {
            if (content[pos] == 'E' && content[pos + 1] == 'I') {
                const bool beforeOk =
                    pos == 0 || isWhitespaceByte(static_cast<unsigned char>(content[pos - 1]));
                const bool afterOk =
                    pos + 2 >= content.size() ||
                    isWhitespaceByte(static_cast<unsigned char>(content[pos + 2])) ||
                    isDelimiterByte(static_cast<unsigned char>(content[pos + 2]));
                if (beforeOk && afterOk) return pos + 2;
            }
            ++pos;
        }
        return content.size();
    }

    PdfDocumentRewriter& document_;
    const ContentRedactionRequest& request_;
    int depth_{0};
    PdfObject resources_{};
    GraphicsState state_{};
    TextState text_{};
    std::vector<GraphicsState> stack_{};
    std::map<std::string, FontMetrics> metricsCache_{};
    ContentRedactionResult result_{};
};

}  // namespace

Matrix multiply(const Matrix& a, const Matrix& b) noexcept {
    return Matrix{a[0] * b[0] + a[1] * b[2],
                  a[0] * b[1] + a[1] * b[3],
                  a[2] * b[0] + a[3] * b[2],
                  a[2] * b[1] + a[3] * b[3],
                  a[4] * b[0] + a[5] * b[2] + b[4],
                  a[4] * b[1] + a[5] * b[3] + b[5]};
}

domain::PointF applyMatrix(const Matrix& m, double x, double y) noexcept {
    return domain::PointF{m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}

domain::RectF transformedBounds(const Matrix& m, const domain::RectF& box) noexcept {
    const domain::RectF r = box.normalized();
    const domain::PointF corners[4] = {applyMatrix(m, r.left, r.bottom),
                                       applyMatrix(m, r.right, r.bottom),
                                       applyMatrix(m, r.left, r.top),
                                       applyMatrix(m, r.right, r.top)};
    double left = corners[0].x;
    double right = corners[0].x;
    double bottom = corners[0].y;
    double top = corners[0].y;
    for (const domain::PointF& p : corners) {
        left = std::min(left, p.x);
        right = std::max(right, p.x);
        bottom = std::min(bottom, p.y);
        top = std::max(top, p.y);
    }
    return domain::RectF{left, bottom, right, top};
}

ContentRedactionResult redactContentStream(PdfDocumentRewriter& document,
                                           const std::string& content, const PdfObject& resources,
                                           const Matrix& baseCtm,
                                           const ContentRedactionRequest& request, int depth) {
    if (depth > kMaxFormDepth) {
        ContentRedactionResult result;
        result.diagnostic = "Form XObject 巢狀過深，拒絕繼續走訪";
        return result;
    }
    StreamRedactor redactor(document, request, depth);
    return redactor.run(content, resources, baseCtm);
}

}  // namespace alioth::engine::redaction
