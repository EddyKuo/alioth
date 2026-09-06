#include "engine/formbuild/expression.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>

namespace alioth::engine::formbuild {

namespace {

// 函式白名單。編譯期固定，執行期無法擴充——這是「引擎能力上限 = 攻擊者能力上限」
// 這條設計原則最直接的體現。要新增函式必須改這裡並補測試。
struct FunctionEntry {
    const char* name;
    int minArgs;
    int maxArgs;  // -1 代表不限（仍受 maxCallArguments 節制）
};

constexpr FunctionEntry kFunctions[] = {
    {"SUM", 1, -1}, {"AVG", 1, -1}, {"MIN", 1, -1},
    {"MAX", 1, -1}, {"ROUND", 1, 2}, {"IF", 3, 3},
};

[[nodiscard]] std::string toUpperAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool isIdentifierStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] bool isIdentifierPart(char c) noexcept {
    return isIdentifierStart(c) || (c >= '0' && c <= '9') || c == '.';
}

[[nodiscard]] bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

}  // namespace

// 詞法型別刻意放在具名的 detail 而不是匿名命名空間：下面的 ExpressionParser
// 是外部連結的類別（Expression 的 friend），持有匿名命名空間的型別會讓
// 連結性混雜，雖然單一翻譯單元下可行，但沒有理由去踩那個邊界。
namespace detail {

enum class TokenKind : std::uint8_t {
    End, Number, Identifier, FieldName, LParen, RParen, Comma,
    Plus, Minus, Star, Slash,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
};

struct Token {
    TokenKind kind{TokenKind::End};
    double number{0.0};
    std::string text;
};

struct LexOutcome {
    bool ok{true};
    ExpressionError error{ExpressionError::None};
    std::string detail;
    std::vector<Token> tokens;
};

// 詞法分析。每一輪都必須至少吃掉一個字元，否則畸形輸入會讓迴圈停不下來——
// 那正是「餵一批畸形字串不得無限迴圈」這條驗收要擋的東西。
[[nodiscard]] LexOutcome lex(std::string_view source, const ExpressionLimits& limits) {
    LexOutcome out;
    std::size_t i = 0;
    while (i < source.size()) {
        const char c = source[i];
        if (isSpace(c)) {
            ++i;
            continue;
        }
        if (out.tokens.size() >= limits.maxTokens) {
            out.ok = false;
            out.error = ExpressionError::TooManyTokens;
            out.detail = "運算式的詞法單元超過上限";
            return out;
        }

        Token token;
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < source.size() &&
                                       source[i + 1] >= '0' && source[i + 1] <= '9')) {
            const std::size_t start = i;
            bool seenDot = false;
            while (i < source.size()) {
                const char d = source[i];
                if (d >= '0' && d <= '9') {
                    ++i;
                } else if (d == '.' && !seenDot) {
                    seenDot = true;
                    ++i;
                } else {
                    break;
                }
            }
            // 刻意不支援科學記號：1e999 只用五個字元就能生出 inf，
            // 而這條文法的每一個特性都要能說出為什麼值得那個風險。
            const std::string_view digits = source.substr(start, i - start);
            double value = 0.0;
            const auto* first = digits.data();
            const auto* last = digits.data() + digits.size();
            const auto result = std::from_chars(first, last, value);
            if (result.ec != std::errc{} || result.ptr != last) {
                out.ok = false;
                out.error = ExpressionError::SyntaxError;
                out.detail = "無法解析的數字：" + std::string(digits);
                return out;
            }
            token.kind = TokenKind::Number;
            token.number = value;
            out.tokens.push_back(std::move(token));
            continue;
        }

        if (isIdentifierStart(c)) {
            const std::size_t start = i;
            while (i < source.size() && isIdentifierPart(source[i])) ++i;
            token.kind = TokenKind::Identifier;
            token.text = std::string(source.substr(start, i - start));
            if (token.text.size() > limits.maxFieldNameLength) {
                out.ok = false;
                out.error = ExpressionError::FieldNameTooLong;
                out.detail = "名稱過長";
                return out;
            }
            out.tokens.push_back(std::move(token));
            continue;
        }

        // 中括號形式的欄位參照。PDF 的欄位名可以含空白與中文，
        // 沒有這個形式就等於強迫使用者改欄位名才能被計算引擎參照。
        if (c == '[') {
            ++i;
            const std::size_t start = i;
            while (i < source.size() && source[i] != ']' && source[i] != '\n' && source[i] != '\r') {
                ++i;
            }
            if (i >= source.size() || source[i] != ']') {
                out.ok = false;
                out.error = ExpressionError::SyntaxError;
                out.detail = "欄位參照的中括號沒有收尾";
                return out;
            }
            token.kind = TokenKind::FieldName;
            token.text = std::string(source.substr(start, i - start));
            ++i;  // 吃掉 ']'
            if (token.text.empty()) {
                out.ok = false;
                out.error = ExpressionError::SyntaxError;
                out.detail = "欄位參照為空";
                return out;
            }
            if (token.text.size() > limits.maxFieldNameLength) {
                out.ok = false;
                out.error = ExpressionError::FieldNameTooLong;
                out.detail = "欄位名稱過長";
                return out;
            }
            out.tokens.push_back(std::move(token));
            continue;
        }

        const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';
        switch (c) {
            case '(': token.kind = TokenKind::LParen; ++i; break;
            case ')': token.kind = TokenKind::RParen; ++i; break;
            case ',': token.kind = TokenKind::Comma; ++i; break;
            case '+': token.kind = TokenKind::Plus; ++i; break;
            case '-': token.kind = TokenKind::Minus; ++i; break;
            case '*': token.kind = TokenKind::Star; ++i; break;
            case '/': token.kind = TokenKind::Slash; ++i; break;
            case '=':
                // 單一 '=' 也視為相等比較。這條文法沒有指派運算，
                // 因此不存在「本來想寫比較卻寫成指派」的歧義。
                token.kind = TokenKind::Equal;
                i += (next == '=') ? 2 : 1;
                break;
            case '<':
                token.kind = (next == '=') ? TokenKind::LessEqual : TokenKind::Less;
                i += (next == '=') ? 2 : 1;
                break;
            case '>':
                token.kind = (next == '=') ? TokenKind::GreaterEqual : TokenKind::Greater;
                i += (next == '=') ? 2 : 1;
                break;
            case '!':
                if (next != '=') {
                    out.ok = false;
                    out.error = ExpressionError::SyntaxError;
                    out.detail = "'!' 後面必須是 '='";
                    return out;
                }
                token.kind = TokenKind::NotEqual;
                i += 2;
                break;
            default:
                out.ok = false;
                out.error = ExpressionError::SyntaxError;
                out.detail = std::string("不認識的字元：'") + c + "'";
                return out;
        }
        out.tokens.push_back(std::move(token));
    }

    Token end;
    end.kind = TokenKind::End;
    out.tokens.push_back(std::move(end));
    return out;
}

}  // namespace detail

namespace {

using detail::LexOutcome;
using detail::Token;
using detail::TokenKind;

}  // namespace

const char* describe(ExpressionError error) noexcept {
    switch (error) {
        case ExpressionError::None: return "成功";
        case ExpressionError::EmptySource: return "運算式為空";
        case ExpressionError::SourceTooLong: return "運算式過長";
        case ExpressionError::TooManyTokens: return "運算式的詞法單元過多";
        case ExpressionError::TooManyNodes: return "運算式過於複雜";
        case ExpressionError::DepthExceeded: return "運算式巢狀過深";
        case ExpressionError::TooManyFieldReferences: return "欄位參照過多";
        case ExpressionError::TooManyArguments: return "函式引數過多";
        case ExpressionError::FieldNameTooLong: return "欄位名稱過長";
        case ExpressionError::SyntaxError: return "語法錯誤";
        case ExpressionError::UnknownFunction: return "不支援的函式";
        case ExpressionError::ArityMismatch: return "函式引數個數不符";
        case ExpressionError::UnknownField: return "參照了不存在的欄位";
        case ExpressionError::TypeMismatch: return "欄位內容不是數字";
        case ExpressionError::DivideByZero: return "除以零";
        case ExpressionError::NotFinite: return "計算結果不是有限數";
        case ExpressionError::CircularReference: return "欄位計算存在循環參照";
    }
    return "未知錯誤";
}

FieldValueSource::~FieldValueSource() = default;

FieldNumber FieldValueSource::fieldNumber(const std::string& name) const {
    FieldNumber out;
    const std::optional<std::string> raw = fieldValue(name);
    if (!raw.has_value()) return out;
    out.present = true;
    out.raw = *raw;
    const NumberParse parsed = parseFieldNumber(*raw);
    out.numeric = parsed.ok;
    out.value = parsed.value;
    return out;
}

void MapFieldValueSource::set(std::string name, std::string value) {
    for (auto& entry : entries_) {
        if (entry.first == name) {
            entry.second = std::move(value);
            return;
        }
    }
    entries_.emplace_back(std::move(name), std::move(value));
}

void MapFieldValueSource::erase(const std::string& name) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const auto& e) { return e.first == name; }),
                   entries_.end());
}

void MapFieldValueSource::clear() { entries_.clear(); }

std::optional<std::string> MapFieldValueSource::fieldValue(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry.first == name) return entry.second;
    }
    return std::nullopt;
}

NumberParse parseFieldNumber(std::string_view text) {
    NumberParse out;

    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char c : text) {
        if (isSpace(c)) continue;
        // 千分位逗號在表單上極常見，留著會讓所有金額欄位都變成型別錯誤。
        if (c == ',') continue;
        cleaned.push_back(c);
    }

    if (cleaned.empty()) {
        out.ok = true;
        out.empty = true;
        out.value = 0.0;
        return out;
    }

    std::string_view body{cleaned};
    double sign = 1.0;
    if (body.front() == '+' || body.front() == '-') {
        if (body.front() == '-') sign = -1.0;
        body.remove_prefix(1);
    }
    if (body.empty()) return out;

    // from_chars 本身會接受 "1e5" 這類寫法，但欄位值的來源是使用者輸入，
    // 保持與運算式文法一致（不含科學記號）比多接受幾種寫法重要。
    for (const char c : body) {
        if (!((c >= '0' && c <= '9') || c == '.')) return out;
    }

    double value = 0.0;
    const auto* first = body.data();
    const auto* last = body.data() + body.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) return out;
    if (!std::isfinite(value)) return out;

    out.ok = true;
    out.value = sign * value;
    return out;
}

// 遞迴下降剖析器。深度只在真正巢狀的地方（括號、函式引數、一元運算）遞增，
// 讓 maxDepth 這個數字對應到使用者看得見的「括號層數」而不是內部的文法層數。
class ExpressionParser {
public:
    ExpressionParser(Expression& target, std::vector<detail::Token> tokens)
        : target_(target), tokens_(std::move(tokens)) {}

    [[nodiscard]] bool run() {
        const int root = parseComparison(0);
        if (root < 0) return false;
        if (peek().kind != TokenKind::End) {
            return fail(ExpressionError::SyntaxError, "運算式尾端有多餘的內容");
        }
        target_.root_ = root;
        return true;
    }

private:
    using NodeKind = Expression::NodeKind;
    using BinaryOp = Expression::BinaryOp;
    using Function = Expression::Function;
    using Node = Expression::Node;

    [[nodiscard]] const detail::Token& peek() const { return tokens_[position_]; }
    void advance() {
        if (tokens_[position_].kind != TokenKind::End) ++position_;
    }

    [[nodiscard]] bool fail(ExpressionError error, std::string detail) {
        target_.error_ = error;
        target_.diagnostic_ = std::move(detail);
        return false;
    }

    [[nodiscard]] int failNode(ExpressionError error, std::string detail) {
        (void)fail(error, std::move(detail));
        return -1;
    }

    [[nodiscard]] int addNode(Node node) {
        if (target_.nodes_.size() >= target_.limits_.maxNodes) {
            return failNode(ExpressionError::TooManyNodes, "運算式的節點數超過上限");
        }
        target_.nodes_.push_back(std::move(node));
        return static_cast<int>(target_.nodes_.size()) - 1;
    }

    [[nodiscard]] int parseComparison(std::size_t depth) {
        const int left = parseAdditive(depth);
        if (left < 0) return -1;

        BinaryOp op{};
        switch (peek().kind) {
            case TokenKind::Equal: op = BinaryOp::Equal; break;
            case TokenKind::NotEqual: op = BinaryOp::NotEqual; break;
            case TokenKind::Less: op = BinaryOp::Less; break;
            case TokenKind::LessEqual: op = BinaryOp::LessEqual; break;
            case TokenKind::Greater: op = BinaryOp::Greater; break;
            case TokenKind::GreaterEqual: op = BinaryOp::GreaterEqual; break;
            default: return left;
        }
        advance();
        const int right = parseAdditive(depth);
        if (right < 0) return -1;

        // 比較不可鏈接（a < b < c 一律視為語法錯誤）。允許鏈接會讓
        // (a<b)<c 這種「先變成 0/1 再比」的結果看起來像是三值比較，
        // 那是靜默算錯，比直接報錯糟糕。
        switch (peek().kind) {
            case TokenKind::Equal:
            case TokenKind::NotEqual:
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                return failNode(ExpressionError::SyntaxError, "比較運算子不可連續使用");
            default:
                break;
        }

        Node node;
        node.kind = NodeKind::Binary;
        node.binary = op;
        node.children = {left, right};
        return addNode(std::move(node));
    }

    [[nodiscard]] int parseAdditive(std::size_t depth) {
        int left = parseMultiplicative(depth);
        if (left < 0) return -1;
        while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
            const BinaryOp op =
                peek().kind == TokenKind::Plus ? BinaryOp::Add : BinaryOp::Sub;
            advance();
            const int right = parseMultiplicative(depth);
            if (right < 0) return -1;
            Node node;
            node.kind = NodeKind::Binary;
            node.binary = op;
            node.children = {left, right};
            left = addNode(std::move(node));
            if (left < 0) return -1;
        }
        return left;
    }

    [[nodiscard]] int parseMultiplicative(std::size_t depth) {
        int left = parseUnary(depth);
        if (left < 0) return -1;
        while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash) {
            const BinaryOp op =
                peek().kind == TokenKind::Star ? BinaryOp::Mul : BinaryOp::Div;
            advance();
            const int right = parseUnary(depth);
            if (right < 0) return -1;
            Node node;
            node.kind = NodeKind::Binary;
            node.binary = op;
            node.children = {left, right};
            left = addNode(std::move(node));
            if (left < 0) return -1;
        }
        return left;
    }

    [[nodiscard]] int parseUnary(std::size_t depth) {
        if (peek().kind == TokenKind::Plus) {
            advance();
            if (depth + 1 > target_.limits_.maxDepth) {
                return failNode(ExpressionError::DepthExceeded, "運算式巢狀過深");
            }
            return parseUnary(depth + 1);
        }
        if (peek().kind == TokenKind::Minus) {
            advance();
            if (depth + 1 > target_.limits_.maxDepth) {
                return failNode(ExpressionError::DepthExceeded, "運算式巢狀過深");
            }
            const int operand = parseUnary(depth + 1);
            if (operand < 0) return -1;
            Node node;
            node.kind = NodeKind::Negate;
            node.children = {operand};
            return addNode(std::move(node));
        }
        return parsePrimary(depth);
    }

    [[nodiscard]] int parsePrimary(std::size_t depth) {
        const detail::Token& token = peek();
        switch (token.kind) {
            case TokenKind::Number: {
                Node node;
                node.kind = NodeKind::Number;
                node.number = token.number;
                advance();
                return addNode(std::move(node));
            }
            case TokenKind::FieldName: {
                std::string name = token.text;
                advance();
                return makeFieldNode(std::move(name));
            }
            case TokenKind::Identifier: {
                std::string name = token.text;
                advance();
                if (peek().kind == TokenKind::LParen) return parseCall(std::move(name), depth);
                return makeFieldNode(std::move(name));
            }
            case TokenKind::LParen: {
                advance();
                if (depth + 1 > target_.limits_.maxDepth) {
                    return failNode(ExpressionError::DepthExceeded, "運算式巢狀過深");
                }
                const int inner = parseComparison(depth + 1);
                if (inner < 0) return -1;
                if (peek().kind != TokenKind::RParen) {
                    return failNode(ExpressionError::SyntaxError, "括號沒有收尾");
                }
                advance();
                return inner;
            }
            case TokenKind::End:
                return failNode(ExpressionError::SyntaxError, "運算式在預期有運算元處結束");
            default:
                return failNode(ExpressionError::SyntaxError, "預期有運算元");
        }
    }

    [[nodiscard]] int makeFieldNode(std::string name) {
        if (++fieldReferenceCount_ > target_.limits_.maxFieldReferences) {
            return failNode(ExpressionError::TooManyFieldReferences, "欄位參照數超過上限");
        }
        Node node;
        node.kind = NodeKind::Field;
        node.name = name;
        const auto it = std::find(target_.referencedFields_.begin(),
                                  target_.referencedFields_.end(), name);
        if (it == target_.referencedFields_.end()) {
            target_.referencedFields_.push_back(std::move(name));
        }
        return addNode(std::move(node));
    }

    [[nodiscard]] int parseCall(std::string rawName, std::size_t depth) {
        const std::string upper = toUpperAscii(rawName);
        const FunctionEntry* entry = nullptr;
        Function function{};
        for (std::size_t i = 0; i < std::size(kFunctions); ++i) {
            if (upper == kFunctions[i].name) {
                entry = &kFunctions[i];
                function = static_cast<Function>(i);
                break;
            }
        }
        if (entry == nullptr) {
            return failNode(ExpressionError::UnknownFunction, "不支援的函式：" + rawName);
        }

        advance();  // '('
        if (depth + 1 > target_.limits_.maxDepth) {
            return failNode(ExpressionError::DepthExceeded, "運算式巢狀過深");
        }

        std::vector<int> args;
        if (peek().kind != TokenKind::RParen) {
            while (true) {
                if (args.size() >= target_.limits_.maxCallArguments) {
                    return failNode(ExpressionError::TooManyArguments,
                                    rawName + " 的引數個數超過上限");
                }
                const int arg = parseComparison(depth + 1);
                if (arg < 0) return -1;
                args.push_back(arg);
                if (peek().kind != TokenKind::Comma) break;
                advance();
            }
        }
        if (peek().kind != TokenKind::RParen) {
            return failNode(ExpressionError::SyntaxError, rawName + " 的括號沒有收尾");
        }
        advance();

        const int count = static_cast<int>(args.size());
        if (count < entry->minArgs || (entry->maxArgs >= 0 && count > entry->maxArgs)) {
            return failNode(ExpressionError::ArityMismatch,
                            std::string(entry->name) + " 的引數個數不符");
        }

        Node node;
        node.kind = NodeKind::Call;
        node.function = function;
        node.name = entry->name;
        node.children = std::move(args);
        return addNode(std::move(node));
    }

    Expression& target_;
    std::vector<detail::Token> tokens_;
    std::size_t position_{0};
    std::size_t fieldReferenceCount_{0};
};

Expression Expression::parse(std::string_view source, const ExpressionLimits& limits) {
    Expression expression;
    expression.limits_ = limits;
    expression.source_.assign(source);

    if (source.size() > limits.maxSourceLength) {
        expression.error_ = ExpressionError::SourceTooLong;
        expression.diagnostic_ = "運算式長度超過上限";
        return expression;
    }
    const bool blank = std::all_of(source.begin(), source.end(),
                                   [](char c) { return isSpace(c); });
    if (blank) {
        expression.error_ = ExpressionError::EmptySource;
        expression.diagnostic_ = "運算式為空";
        return expression;
    }

    detail::LexOutcome lexed = detail::lex(source, limits);
    if (!lexed.ok) {
        expression.error_ = lexed.error;
        expression.diagnostic_ = std::move(lexed.detail);
        return expression;
    }

    expression.error_ = ExpressionError::None;
    expression.diagnostic_.clear();
    ExpressionParser parser(expression, std::move(lexed.tokens));
    if (!parser.run()) {
        expression.root_ = -1;
        if (expression.error_ == ExpressionError::None) {
            expression.error_ = ExpressionError::SyntaxError;
            expression.diagnostic_ = "語法錯誤";
        }
        expression.referencedFields_.clear();
        return expression;
    }
    return expression;
}

namespace {

[[nodiscard]] EvalOutcome evalFailure(ExpressionError error, std::string detail) {
    EvalOutcome out;
    out.error = error;
    out.detail = std::move(detail);
    return out;
}

[[nodiscard]] EvalOutcome evalSuccess(double value) {
    EvalOutcome out;
    out.ok = true;
    out.value = value;
    return out;
}

}  // namespace

EvalOutcome Expression::evaluate(const FieldValueSource& values) const {
    if (!valid()) return evalFailure(error_, diagnostic_.empty() ? describe(error_) : diagnostic_);
    return evaluateNode(root_, values, 0);
}

EvalOutcome Expression::evaluateNode(int index, const FieldValueSource& values,
                                     std::size_t depth) const {
    if (depth > limits_.maxDepth) {
        return evalFailure(ExpressionError::DepthExceeded, "求值巢狀過深");
    }
    if (index < 0 || static_cast<std::size_t>(index) >= nodes_.size()) {
        return evalFailure(ExpressionError::SyntaxError, "節點索引超出範圍");
    }
    const Node& node = nodes_[static_cast<std::size_t>(index)];

    switch (node.kind) {
        case NodeKind::Number:
            return evalSuccess(node.number);

        case NodeKind::Field: {
            const FieldNumber number = values.fieldNumber(node.name);
            if (!number.present) {
                return evalFailure(ExpressionError::UnknownField, "找不到欄位：" + node.name);
            }
            if (!number.numeric) {
                return evalFailure(ExpressionError::TypeMismatch,
                                   "欄位「" + node.name + "」的內容不是數字：" + number.raw);
            }
            return evalSuccess(number.value);
        }

        case NodeKind::Negate: {
            const EvalOutcome operand = evaluateNode(node.children[0], values, depth + 1);
            if (!operand.ok) return operand;
            return evalSuccess(-operand.value);
        }

        case NodeKind::Binary: {
            const EvalOutcome left = evaluateNode(node.children[0], values, depth + 1);
            if (!left.ok) return left;
            const EvalOutcome right = evaluateNode(node.children[1], values, depth + 1);
            if (!right.ok) return right;

            double value = 0.0;
            switch (node.binary) {
                case BinaryOp::Add: value = left.value + right.value; break;
                case BinaryOp::Sub: value = left.value - right.value; break;
                case BinaryOp::Mul: value = left.value * right.value; break;
                case BinaryOp::Div:
                    // 明確報錯而不是回傳 inf：表單上的 inf 會一路傳染到別的欄位，
                    // 使用者看到的是一整排空白，而不是「某一格的分母是零」。
                    if (right.value == 0.0) {
                        return evalFailure(ExpressionError::DivideByZero, "除數為零");
                    }
                    value = left.value / right.value;
                    break;
                case BinaryOp::Equal: value = (left.value == right.value) ? 1.0 : 0.0; break;
                case BinaryOp::NotEqual: value = (left.value != right.value) ? 1.0 : 0.0; break;
                case BinaryOp::Less: value = (left.value < right.value) ? 1.0 : 0.0; break;
                case BinaryOp::LessEqual: value = (left.value <= right.value) ? 1.0 : 0.0; break;
                case BinaryOp::Greater: value = (left.value > right.value) ? 1.0 : 0.0; break;
                case BinaryOp::GreaterEqual: value = (left.value >= right.value) ? 1.0 : 0.0; break;
            }
            if (!std::isfinite(value)) {
                return evalFailure(ExpressionError::NotFinite, "計算結果溢位");
            }
            return evalSuccess(value);
        }

        case NodeKind::Call:
            return evaluateCall(node, values, depth);
    }
    return evalFailure(ExpressionError::SyntaxError, "未知的節點型別");
}

EvalOutcome Expression::evaluateCall(const Node& node, const FieldValueSource& values,
                                     std::size_t depth) const {
    // IF 的分支必須惰性求值：只算被選中的那一支。
    // 否則 IF(qty = 0, 0, total / qty) 這種最常見的防呆寫法會在分母為零時
    // 直接失敗——而使用者寫那條式子的目的正是避免這件事。
    if (node.function == Function::If) {
        const EvalOutcome condition = evaluateNode(node.children[0], values, depth + 1);
        if (!condition.ok) return condition;
        const int branch = (condition.value != 0.0) ? node.children[1] : node.children[2];
        return evaluateNode(branch, values, depth + 1);
    }

    std::vector<double> args;
    args.reserve(node.children.size());
    for (const int child : node.children) {
        const EvalOutcome arg = evaluateNode(child, values, depth + 1);
        if (!arg.ok) return arg;
        args.push_back(arg.value);
    }

    double value = 0.0;
    switch (node.function) {
        case Function::Sum:
            for (const double a : args) value += a;
            break;
        case Function::Avg: {
            for (const double a : args) value += a;
            // 分母是「引數個數」而不是「非空欄位個數」。空欄位當成 0 一起算，
            // 與 Acrobat 的 AVG 一致；不一致的話同一份表單在兩邊會算出不同的平均。
            value /= static_cast<double>(args.size());
            break;
        }
        case Function::Min:
            value = *std::min_element(args.begin(), args.end());
            break;
        case Function::Max:
            value = *std::max_element(args.begin(), args.end());
            break;
        case Function::Round: {
            const double digits = (args.size() == 2) ? args[1] : 0.0;
            if (digits < 0.0 || digits > 10.0 || digits != std::floor(digits)) {
                return evalFailure(ExpressionError::ArityMismatch,
                                   "ROUND 的小數位數必須是 0 到 10 的整數");
            }
            const double scale = std::pow(10.0, digits);
            // std::round 是「遠離零的四捨五入」，與表單使用者的直覺一致；
            // std::nearbyint 的預設模式是銀行家捨入，金額欄位上會出現
            // 「明明是 .5 卻沒進位」的客訴。
            value = std::round(args[0] * scale) / scale;
            break;
        }
        case Function::If:
            break;  // 上面已提前回傳
    }

    if (!std::isfinite(value)) {
        return evalFailure(ExpressionError::NotFinite, node.name + " 的結果溢位");
    }
    return evalSuccess(value);
}

}  // namespace alioth::engine::formbuild
