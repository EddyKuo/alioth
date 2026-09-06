#pragma once

// 巨集系統的動作綱要（PRD-MAC-001，WBS 批次處理）。
//
// **絕對法則：巨集不是腳本，是一串具名、參數化、可序列化的內建動作。**
// 這不是效能或簡潔度的取捨，是安全立場的直接推論：專案對 PDF 本身都採
// 「視為不可信任輸入、不執行內嵌 JavaScript」的態度（PRD §8.2），
// 若巨集系統本身是圖靈完備的腳本語言，等於在使用者機器上開了一個新的、
// 完全不受那條規則約束的執行環境——使用者從網路上下載一份「別人做好的
// 巨集」來跑，風險與執行一份未知來源的 exe 沒有本質差異。
//
// 因此每一個 MacroAction 都必須對應到一個「本產品已經實作、已經測過」的
// 服務呼叫（旋轉頁面、色彩轉換、貼條碼……），新增動作種類是程式碼變更
// （需要走過本產品自己的測試與審查），不是使用者可以自行擴充的東西。
// 這個限制同時解釋了為什麼 kActionKinds 目前只有三種：批次處理最常見的
// 「對一批檔案做同一件事」，這三種已經覆蓋了 WP35 其餘需求（ENH-006、
// ENH-007）已經實作的服務；其餘動作種類留待後續工作包依實際需求擴充，
// 而不是預先猜測使用者想要什麼。
//
// header-only：與 domain/document_source.h 同一個理由，純資料與純函數，
// 沒有跨翻譯單元的狀態。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain/annotation.h"
#include "domain/bookmark_ops.h"
#include "domain/enhance.h"
#include "domain/geometry.h"

namespace alioth::domain::macro {

// PRD-BM-020「進階書籤巨集」的落實方式：不是另建第二套巨集機制，而是把
// bookmark_ops.h 既有的、已經測過的批次操作各自包成一個 MacroActionKind，
// 接進同一套 MacroDefinition／BatchRunner。這五種涵蓋 PRD-BM-002（標題加
// 文字）、PRD-BM-003（每 N 頁加書籤）、PRD-BM-004（大小寫轉換）、
// PRD-BM-011（標題尋找取代）、PRD-BM-016（合併重複書籤）——選這五種是因為
// 它們都是「對整份文件的書籤樹做一次變換」，語意上與巨集系統既有三種動作
// （對整份文件的頁面/色彩/條碼做一次變換）完全對稱，可以直接套進
// app::BatchRunner 現有的「開檔 → 依序套用步驟 → 存檔」流程，不需要引入
// 任何新的執行模型。PRD-BM-017（移除動作）刻意不做成巨集步驟：它會讓書籤
// 樹裡原本能跳轉的節點全部失去目標，在批次情境下一步做錯就是整批檔案的
// 書籤全部變成不能點，風險與其餘五種不對稱，需要更高的操作門檻（例如
// 逐檔案確認），因此留在互動式的書籤面板操作，不進巨集。
enum class MacroActionKind : std::uint8_t {
    RotatePages,
    ConvertColor,
    AddBarcodeStamp,
    BookmarkAddAffix,
    BookmarkEveryNPages,
    BookmarkConvertCase,
    BookmarkFindReplace,
    BookmarkMergeDuplicates,
};

[[nodiscard]] inline const char* actionKindName(MacroActionKind kind) noexcept {
    switch (kind) {
        case MacroActionKind::RotatePages: return "RotatePages";
        case MacroActionKind::ConvertColor: return "ConvertColor";
        case MacroActionKind::AddBarcodeStamp: return "AddBarcodeStamp";
        case MacroActionKind::BookmarkAddAffix: return "BookmarkAddAffix";
        case MacroActionKind::BookmarkEveryNPages: return "BookmarkEveryNPages";
        case MacroActionKind::BookmarkConvertCase: return "BookmarkConvertCase";
        case MacroActionKind::BookmarkFindReplace: return "BookmarkFindReplace";
        case MacroActionKind::BookmarkMergeDuplicates: return "BookmarkMergeDuplicates";
    }
    return "Unknown";
}

[[nodiscard]] inline std::optional<MacroActionKind> parseActionKind(std::string_view name) noexcept {
    if (name == "RotatePages") return MacroActionKind::RotatePages;
    if (name == "ConvertColor") return MacroActionKind::ConvertColor;
    if (name == "AddBarcodeStamp") return MacroActionKind::AddBarcodeStamp;
    if (name == "BookmarkAddAffix") return MacroActionKind::BookmarkAddAffix;
    if (name == "BookmarkEveryNPages") return MacroActionKind::BookmarkEveryNPages;
    if (name == "BookmarkConvertCase") return MacroActionKind::BookmarkConvertCase;
    if (name == "BookmarkFindReplace") return MacroActionKind::BookmarkFindReplace;
    if (name == "BookmarkMergeDuplicates") return MacroActionKind::BookmarkMergeDuplicates;
    return std::nullopt;
}

struct MacroAction {
    MacroActionKind kind{MacroActionKind::RotatePages};

    // 空代表套用到全部頁面（與 domain::enhance::ColorTransformSettings::pages
    // 的慣例一致）。0 起算。
    std::vector<std::int32_t> pages{};

    // RotatePages：90/180/270（順時針），其餘動作忽略此欄。
    int rotationDegrees{90};

    // ConvertColor：重用既有的色彩轉換設定，避免同一組語意（灰階／去飽和／
    // 換色）在巨集層再定義一次、兩邊定義漂移。
    domain::enhance::ColorTransformSettings colorSettings{};

    // AddBarcodeStamp。
    std::string barcodeText;
    domain::RectF barcodeRect{};

    // BookmarkAddAffix：直接重用 bookmark_ops.h 的 AffixOptions，
    // 巨集層不重新定義同一組欄位，避免兩邊語意漂移。
    domain::bookmarks::AffixOptions bookmarkAffix{};

    // BookmarkEveryNPages：pageCount 不在這裡填——它要等到巨集實際套用到
    // 某一份檔案、開了檔才知道頁數，因此由 batch_runner 在套用當下補上，
    // 這裡只存使用者可以事先決定的部分。
    std::int32_t bookmarkInterval{1};
    std::int32_t bookmarkFirstPage{0};
    std::int32_t bookmarkPageLabelOffset{1};
    std::string bookmarkTitlePattern{"Page {page}"};

    // BookmarkConvertCase。
    domain::bookmarks::CaseMode bookmarkCaseMode{domain::bookmarks::CaseMode::TitleCase};

    // BookmarkFindReplace：直接重用 bookmark_ops.h 的 FindReplaceOptions。
    domain::bookmarks::FindReplaceOptions bookmarkFindReplace{};

    // BookmarkMergeDuplicates：直接重用 bookmark_ops.h 的
    // MergeDuplicatesOptions。
    domain::bookmarks::MergeDuplicatesOptions bookmarkMergeDuplicates{};

    [[nodiscard]] std::string validate() const {
        switch (kind) {
            case MacroActionKind::RotatePages:
                if (rotationDegrees != 90 && rotationDegrees != 180 && rotationDegrees != 270 &&
                    rotationDegrees != -90 && rotationDegrees != -180 && rotationDegrees != -270) {
                    return "RotatePages 的角度必須是 90/180/270 或其負值";
                }
                return {};
            case MacroActionKind::ConvertColor:
                if (!colorSettings.valid()) return "ConvertColor 的設定不合法";
                return {};
            case MacroActionKind::AddBarcodeStamp:
                if (barcodeText.empty()) return "AddBarcodeStamp 的文字不可為空";
                if (barcodeRect.normalized().isEmpty()) return "AddBarcodeStamp 的矩形不可為空";
                return {};
            case MacroActionKind::BookmarkAddAffix:
                if (bookmarkAffix.prefix.empty() && bookmarkAffix.suffix.empty()) {
                    return "BookmarkAddAffix 的前綴與後綴不可同時為空";
                }
                return {};
            case MacroActionKind::BookmarkEveryNPages:
                if (bookmarkInterval <= 0) return "BookmarkEveryNPages 的間隔必須大於 0";
                return {};
            case MacroActionKind::BookmarkConvertCase:
                return {};
            case MacroActionKind::BookmarkFindReplace:
                if (bookmarkFindReplace.find.empty()) return "BookmarkFindReplace 的尋找字串不可為空";
                return {};
            case MacroActionKind::BookmarkMergeDuplicates:
                return {};
        }
        return "未知的動作種類";
    }
};

struct MacroDefinition {
    std::string name;
    std::vector<MacroAction> steps;

    [[nodiscard]] std::string validate() const {
        if (name.empty()) return "巨集名稱不可為空";
        if (steps.empty()) return "巨集至少要有一個動作";
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const std::string problem = steps[i].validate();
            if (!problem.empty()) {
                return "第 " + std::to_string(i + 1) + " 個動作（" +
                       actionKindName(steps[i].kind) + "）不合法：" + problem;
            }
        }
        return {};
    }
};

// ---------------------------------------------------------------------------
// 最小 JSON 序列化／解析。
//
// 不引入第三方 JSON 函式庫（PRD §4.1 固定三件引擎級元件，巨集定義檔不算
// 引擎級功能，不值得為它多背一個相依）；手寫的範圍刻意限制在本綱要用得到
// 的子集：物件、陣列、字串（含常見跳脫）、數字、true/false/null。
// 這與 domain/csv.h 手寫 RFC 4180 是同一個決策模式。
// ---------------------------------------------------------------------------

namespace json {

enum class ValueKind { Null, Bool, Number, String, Array, Object };

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // 保序，欄位不多，線性查找足夠

struct Value {
    ValueKind kind{ValueKind::Null};
    bool boolValue{false};
    double numberValue{0.0};
    std::string stringValue;
    Array arrayValue;
    Object objectValue;

    [[nodiscard]] static Value makeString(std::string s) {
        Value v; v.kind = ValueKind::String; v.stringValue = std::move(s); return v;
    }
    [[nodiscard]] static Value makeNumber(double d) {
        Value v; v.kind = ValueKind::Number; v.numberValue = d; return v;
    }
    [[nodiscard]] static Value makeBool(bool b) {
        Value v; v.kind = ValueKind::Bool; v.boolValue = b; return v;
    }
    [[nodiscard]] static Value makeArray(Array a) {
        Value v; v.kind = ValueKind::Array; v.arrayValue = std::move(a); return v;
    }
    [[nodiscard]] static Value makeObject(Object o) {
        Value v; v.kind = ValueKind::Object; v.objectValue = std::move(o); return v;
    }

    // 刻意用 .first/.second 而不是結構化綁定：本檔會被含 Q_OBJECT 的測試
    // 間接引入，而部分版本的 moc 對 `for (auto& [a, b] : ...)` 這種語法
    // 解析不完整，會靜默略過整份檔案裡的 Q_OBJECT 巨集（症狀是連結期缺
    // metaObject／qt_metacast 符號，而不是編譯期錯誤，非常難聯想到原因）。
    [[nodiscard]] const Value* find(const std::string& key) const noexcept {
        if (kind != ValueKind::Object) return nullptr;
        for (const auto& entry : objectValue) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

[[nodiscard]] inline std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] inline std::string serialize(const Value& value) {
    switch (value.kind) {
        case ValueKind::Null: return "null";
        case ValueKind::Bool: return value.boolValue ? "true" : "false";
        case ValueKind::Number: {
            // 整數值不印小數點，讓輸出的巨集檔案人眼可讀（頁碼、角度都是整數）。
            if (value.numberValue == static_cast<long long>(value.numberValue)) {
                return std::to_string(static_cast<long long>(value.numberValue));
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6g", value.numberValue);
            return buf;
        }
        case ValueKind::String: return escapeString(value.stringValue);
        case ValueKind::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.arrayValue.size(); ++i) {
                if (i > 0) out += ",";
                out += serialize(value.arrayValue[i]);
            }
            out += "]";
            return out;
        }
        case ValueKind::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < value.objectValue.size(); ++i) {
                if (i > 0) out += ",";
                out += escapeString(value.objectValue[i].first);
                out += ":";
                out += serialize(value.objectValue[i].second);
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

// 遞迴下降解析器。輸入是不可信任的（使用者可能手改巨集檔），任何格式錯誤
// 一律回傳 ok=false 並附上位置，不丟例外、不嘗試「猜測使用者想表達什麼」。
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<Value> parse(std::string* diagnostic) {
        skipWhitespace();
        std::optional<Value> value = parseValue(diagnostic);
        if (!value.has_value()) return std::nullopt;
        skipWhitespace();
        if (pos_ != text_.size()) {
            if (diagnostic != nullptr) *diagnostic = "JSON 結尾有多餘內容";
            return std::nullopt;
        }
        return value;
    }

private:
    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            break;
        }
    }

    // 不標 [[nodiscard]]：呼叫端多半只是要記錄診斷訊息再各自 return std::nullopt，
    // 不需要每次都接住這個回傳值——它恆為 false，只是為了讓部分呼叫點可以寫成
    // `return fail(...);` 的簡寫。
    bool fail(std::string* diagnostic, const std::string& message) const {
        if (diagnostic != nullptr) {
            *diagnostic = message + "（位置 " + std::to_string(pos_) + "）";
        }
        return false;
    }

    std::optional<Value> parseValue(std::string* diagnostic) {
        skipWhitespace();
        if (pos_ >= text_.size()) { fail(diagnostic, "預期一個值卻遇到結尾"); return std::nullopt; }
        const char c = text_[pos_];
        if (c == '{') return parseObject(diagnostic);
        if (c == '[') return parseArray(diagnostic);
        if (c == '"') return parseString(diagnostic);
        if (c == 't' || c == 'f') return parseBool(diagnostic);
        if (c == 'n') return parseNull(diagnostic);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(diagnostic);
        fail(diagnostic, std::string("無法辨識的字元：") + c);
        return std::nullopt;
    }

    std::optional<Value> parseObject(std::string* diagnostic) {
        ++pos_;  // '{'
        Object object;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return Value::makeObject({}); }
        while (true) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail(diagnostic, "物件的鍵必須是字串");
                return std::nullopt;
            }
            std::optional<Value> key = parseString(diagnostic);
            if (!key.has_value()) return std::nullopt;
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                fail(diagnostic, "預期 ':'");
                return std::nullopt;
            }
            ++pos_;
            std::optional<Value> value = parseValue(diagnostic);
            if (!value.has_value()) return std::nullopt;
            object.emplace_back(key->stringValue, std::move(*value));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; break; }
            fail(diagnostic, "預期 ',' 或 '}'");
            return std::nullopt;
        }
        return Value::makeObject(std::move(object));
    }

    std::optional<Value> parseArray(std::string* diagnostic) {
        ++pos_;  // '['
        Array array;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return Value::makeArray({}); }
        while (true) {
            std::optional<Value> value = parseValue(diagnostic);
            if (!value.has_value()) return std::nullopt;
            array.push_back(std::move(*value));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; break; }
            fail(diagnostic, "預期 ',' 或 ']'");
            return std::nullopt;
        }
        return Value::makeArray(std::move(array));
    }

    std::optional<Value> parseString(std::string* diagnostic) {
        ++pos_;  // '"'
        std::string out;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            char c = text_[pos_++];
            if (c == '\\') {
                if (pos_ >= text_.size()) { fail(diagnostic, "字串在跳脫序列處結束"); return std::nullopt; }
                const char escaped = text_[pos_++];
                switch (escaped) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) { fail(diagnostic, "\\u 跳脫長度不足"); return std::nullopt; }
                        int value = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = text_[pos_++];
                            int digit = 0;
                            if (h >= '0' && h <= '9') digit = h - '0';
                            else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                            else { fail(diagnostic, "\\u 跳脫含非十六進位字元"); return std::nullopt; }
                            value = value * 16 + digit;
                        }
                        // 僅保留 ASCII 範圍：本綱要的字串內容（巨集名稱、條碼文字）
                        // 一律要求可列印 ASCII，非 ASCII 的 \u 跳脫在這裡就明確
                        // 標記為不可表示，而不是輸出未定義的 UTF-8 位元組序列。
                        out.push_back(value < 128 ? static_cast<char>(value)
                                                  : static_cast<char>(0xFF));
                        break;
                    }
                    default:
                        fail(diagnostic, "不支援的跳脫字元");
                        return std::nullopt;
                }
                continue;
            }
            out.push_back(c);
        }
        if (pos_ >= text_.size()) { fail(diagnostic, "字串沒有結尾的引號"); return std::nullopt; }
        ++pos_;  // 結尾的 '"'
        return Value::makeString(std::move(out));
    }

    std::optional<Value> parseBool(std::string* diagnostic) {
        if (text_.compare(pos_, 4, "true") == 0) { pos_ += 4; return Value::makeBool(true); }
        if (text_.compare(pos_, 5, "false") == 0) { pos_ += 5; return Value::makeBool(false); }
        fail(diagnostic, "無法辨識的字面值");
        return std::nullopt;
    }

    std::optional<Value> parseNull(std::string* diagnostic) {
        if (text_.compare(pos_, 4, "null") == 0) { pos_ += 4; return Value{}; }
        fail(diagnostic, "無法辨識的字面值");
        return std::nullopt;
    }

    std::optional<Value> parseNumber(std::string* diagnostic) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ == start) { fail(diagnostic, "無法辨識的數字"); return std::nullopt; }
        const std::string token(text_.substr(start, pos_ - start));
        return Value::makeNumber(std::strtod(token.c_str(), nullptr));
    }

    std::string_view text_;
    std::size_t pos_{0};
};

[[nodiscard]] inline std::optional<Value> parse(std::string_view text, std::string* diagnostic) {
    Parser parser(text);
    return parser.parse(diagnostic);
}

}  // namespace json

// ---------------------------------------------------------------------------
// MacroDefinition <-> JSON
// ---------------------------------------------------------------------------

[[nodiscard]] inline json::Value colorModeToJson(domain::enhance::ColorTransformMode mode) {
    switch (mode) {
        case domain::enhance::ColorTransformMode::Grayscale:
            return json::Value::makeString("Grayscale");
        case domain::enhance::ColorTransformMode::Desaturate:
            return json::Value::makeString("Desaturate");
        case domain::enhance::ColorTransformMode::ReplaceColor:
            return json::Value::makeString("ReplaceColor");
    }
    return json::Value::makeString("Grayscale");
}

[[nodiscard]] inline std::optional<domain::enhance::ColorTransformMode> colorModeFromString(
    const std::string& text) {
    if (text == "Grayscale") return domain::enhance::ColorTransformMode::Grayscale;
    if (text == "Desaturate") return domain::enhance::ColorTransformMode::Desaturate;
    if (text == "ReplaceColor") return domain::enhance::ColorTransformMode::ReplaceColor;
    return std::nullopt;
}

[[nodiscard]] inline json::Value colorToJson(const domain::ColorRgb& c) {
    json::Object o;
    o.emplace_back("r", json::Value::makeNumber(c.r));
    o.emplace_back("g", json::Value::makeNumber(c.g));
    o.emplace_back("b", json::Value::makeNumber(c.b));
    return json::Value::makeObject(std::move(o));
}

[[nodiscard]] inline domain::ColorRgb colorFromJson(const json::Value& v) {
    domain::ColorRgb c;
    if (const json::Value* r = v.find("r"); r != nullptr) c.r = r->numberValue;
    if (const json::Value* g = v.find("g"); g != nullptr) c.g = g->numberValue;
    if (const json::Value* b = v.find("b"); b != nullptr) c.b = b->numberValue;
    return c;
}

[[nodiscard]] inline json::Value caseModeToJson(domain::bookmarks::CaseMode mode) {
    switch (mode) {
        case domain::bookmarks::CaseMode::Upper: return json::Value::makeString("Upper");
        case domain::bookmarks::CaseMode::Lower: return json::Value::makeString("Lower");
        case domain::bookmarks::CaseMode::TitleCase: return json::Value::makeString("TitleCase");
        case domain::bookmarks::CaseMode::SentenceCase: return json::Value::makeString("SentenceCase");
    }
    return json::Value::makeString("TitleCase");
}

[[nodiscard]] inline std::optional<domain::bookmarks::CaseMode> caseModeFromString(
    const std::string& text) {
    if (text == "Upper") return domain::bookmarks::CaseMode::Upper;
    if (text == "Lower") return domain::bookmarks::CaseMode::Lower;
    if (text == "TitleCase") return domain::bookmarks::CaseMode::TitleCase;
    if (text == "SentenceCase") return domain::bookmarks::CaseMode::SentenceCase;
    return std::nullopt;
}

[[nodiscard]] inline json::Value pagesToJson(const std::vector<std::int32_t>& pages) {
    json::Array arr;
    arr.reserve(pages.size());
    for (const std::int32_t p : pages) arr.push_back(json::Value::makeNumber(p));
    return json::Value::makeArray(std::move(arr));
}

[[nodiscard]] inline std::vector<std::int32_t> pagesFromJson(const json::Value* v) {
    std::vector<std::int32_t> pages;
    if (v == nullptr || v->kind != json::ValueKind::Array) return pages;
    for (const json::Value& item : v->arrayValue) pages.push_back(static_cast<std::int32_t>(item.numberValue));
    return pages;
}

[[nodiscard]] inline json::Value actionToJson(const MacroAction& action) {
    json::Object o;
    o.emplace_back("action", json::Value::makeString(actionKindName(action.kind)));
    o.emplace_back("pages", pagesToJson(action.pages));
    switch (action.kind) {
        case MacroActionKind::RotatePages:
            o.emplace_back("rotationDegrees", json::Value::makeNumber(action.rotationDegrees));
            break;
        case MacroActionKind::ConvertColor:
            o.emplace_back("mode", colorModeToJson(action.colorSettings.mode));
            o.emplace_back("desaturateAmount",
                           json::Value::makeNumber(action.colorSettings.desaturateAmount));
            o.emplace_back("replaceFrom", colorToJson(action.colorSettings.replaceFrom));
            o.emplace_back("replaceTo", colorToJson(action.colorSettings.replaceTo));
            o.emplace_back("replaceTolerance",
                           json::Value::makeNumber(action.colorSettings.replaceTolerance));
            break;
        case MacroActionKind::AddBarcodeStamp: {
            o.emplace_back("text", json::Value::makeString(action.barcodeText));
            json::Object rect;
            rect.emplace_back("left", json::Value::makeNumber(action.barcodeRect.left));
            rect.emplace_back("bottom", json::Value::makeNumber(action.barcodeRect.bottom));
            rect.emplace_back("right", json::Value::makeNumber(action.barcodeRect.right));
            rect.emplace_back("top", json::Value::makeNumber(action.barcodeRect.top));
            o.emplace_back("rect", json::Value::makeObject(std::move(rect)));
            break;
        }
        case MacroActionKind::BookmarkAddAffix:
            o.emplace_back("prefix", json::Value::makeString(action.bookmarkAffix.prefix));
            o.emplace_back("suffix", json::Value::makeString(action.bookmarkAffix.suffix));
            if (action.bookmarkAffix.level.has_value()) {
                o.emplace_back("level", json::Value::makeNumber(*action.bookmarkAffix.level));
            }
            o.emplace_back("skipEmptyTitles",
                           json::Value::makeBool(action.bookmarkAffix.skipEmptyTitles));
            o.emplace_back("containing", json::Value::makeString(action.bookmarkAffix.containing));
            o.emplace_back("caseSensitive",
                           json::Value::makeBool(action.bookmarkAffix.caseSensitive));
            break;
        case MacroActionKind::BookmarkEveryNPages:
            o.emplace_back("interval", json::Value::makeNumber(action.bookmarkInterval));
            o.emplace_back("firstPage", json::Value::makeNumber(action.bookmarkFirstPage));
            o.emplace_back("pageLabelOffset",
                           json::Value::makeNumber(action.bookmarkPageLabelOffset));
            o.emplace_back("titlePattern", json::Value::makeString(action.bookmarkTitlePattern));
            break;
        case MacroActionKind::BookmarkConvertCase:
            o.emplace_back("mode", caseModeToJson(action.bookmarkCaseMode));
            break;
        case MacroActionKind::BookmarkFindReplace:
            o.emplace_back("find", json::Value::makeString(action.bookmarkFindReplace.find));
            o.emplace_back("replace", json::Value::makeString(action.bookmarkFindReplace.replace));
            o.emplace_back("caseSensitive",
                           json::Value::makeBool(action.bookmarkFindReplace.caseSensitive));
            o.emplace_back("wholeWord", json::Value::makeBool(action.bookmarkFindReplace.wholeWord));
            o.emplace_back("firstOccurrenceOnly",
                           json::Value::makeBool(action.bookmarkFindReplace.firstOccurrenceOnly));
            break;
        case MacroActionKind::BookmarkMergeDuplicates:
            o.emplace_back("caseSensitive",
                           json::Value::makeBool(action.bookmarkMergeDuplicates.caseSensitive));
            o.emplace_back("requireSameTarget",
                           json::Value::makeBool(action.bookmarkMergeDuplicates.requireSameTarget));
            o.emplace_back("mergeChildren",
                           json::Value::makeBool(action.bookmarkMergeDuplicates.mergeChildren));
            break;
    }
    return json::Value::makeObject(std::move(o));
}

[[nodiscard]] inline std::optional<MacroAction> actionFromJson(const json::Value& value,
                                                                std::string* diagnostic) {
    const json::Value* actionName = value.find("action");
    if (actionName == nullptr || actionName->kind != json::ValueKind::String) {
        if (diagnostic != nullptr) *diagnostic = "動作缺少 \"action\" 欄位";
        return std::nullopt;
    }
    const std::optional<MacroActionKind> kind = parseActionKind(actionName->stringValue);
    if (!kind.has_value()) {
        if (diagnostic != nullptr) *diagnostic = "無法辨識的動作種類：" + actionName->stringValue;
        return std::nullopt;
    }

    MacroAction action;
    action.kind = *kind;
    action.pages = pagesFromJson(value.find("pages"));

    switch (*kind) {
        case MacroActionKind::RotatePages:
            if (const json::Value* v = value.find("rotationDegrees"); v != nullptr) {
                action.rotationDegrees = static_cast<int>(v->numberValue);
            }
            break;
        case MacroActionKind::ConvertColor: {
            if (const json::Value* v = value.find("mode"); v != nullptr) {
                const std::optional<domain::enhance::ColorTransformMode> mode =
                    colorModeFromString(v->stringValue);
                if (!mode.has_value()) {
                    if (diagnostic != nullptr) *diagnostic = "無法辨識的色彩轉換模式：" + v->stringValue;
                    return std::nullopt;
                }
                action.colorSettings.mode = *mode;
            }
            if (const json::Value* v = value.find("desaturateAmount"); v != nullptr) {
                action.colorSettings.desaturateAmount = v->numberValue;
            }
            if (const json::Value* v = value.find("replaceFrom"); v != nullptr) {
                action.colorSettings.replaceFrom = colorFromJson(*v);
            }
            if (const json::Value* v = value.find("replaceTo"); v != nullptr) {
                action.colorSettings.replaceTo = colorFromJson(*v);
            }
            if (const json::Value* v = value.find("replaceTolerance"); v != nullptr) {
                action.colorSettings.replaceTolerance = v->numberValue;
            }
            break;
        }
        case MacroActionKind::AddBarcodeStamp: {
            if (const json::Value* v = value.find("text"); v != nullptr) action.barcodeText = v->stringValue;
            if (const json::Value* v = value.find("rect"); v != nullptr) {
                if (const json::Value* left = v->find("left"); left != nullptr) action.barcodeRect.left = left->numberValue;
                if (const json::Value* bottom = v->find("bottom"); bottom != nullptr) action.barcodeRect.bottom = bottom->numberValue;
                if (const json::Value* right = v->find("right"); right != nullptr) action.barcodeRect.right = right->numberValue;
                if (const json::Value* top = v->find("top"); top != nullptr) action.barcodeRect.top = top->numberValue;
            }
            break;
        }
        case MacroActionKind::BookmarkAddAffix:
            if (const json::Value* v = value.find("prefix"); v != nullptr) action.bookmarkAffix.prefix = v->stringValue;
            if (const json::Value* v = value.find("suffix"); v != nullptr) action.bookmarkAffix.suffix = v->stringValue;
            if (const json::Value* v = value.find("level"); v != nullptr) {
                action.bookmarkAffix.level = static_cast<int>(v->numberValue);
            }
            if (const json::Value* v = value.find("skipEmptyTitles"); v != nullptr) {
                action.bookmarkAffix.skipEmptyTitles = v->boolValue;
            }
            if (const json::Value* v = value.find("containing"); v != nullptr) {
                action.bookmarkAffix.containing = v->stringValue;
            }
            if (const json::Value* v = value.find("caseSensitive"); v != nullptr) {
                action.bookmarkAffix.caseSensitive = v->boolValue;
            }
            break;
        case MacroActionKind::BookmarkEveryNPages:
            if (const json::Value* v = value.find("interval"); v != nullptr) {
                action.bookmarkInterval = static_cast<std::int32_t>(v->numberValue);
            }
            if (const json::Value* v = value.find("firstPage"); v != nullptr) {
                action.bookmarkFirstPage = static_cast<std::int32_t>(v->numberValue);
            }
            if (const json::Value* v = value.find("pageLabelOffset"); v != nullptr) {
                action.bookmarkPageLabelOffset = static_cast<std::int32_t>(v->numberValue);
            }
            if (const json::Value* v = value.find("titlePattern"); v != nullptr) {
                action.bookmarkTitlePattern = v->stringValue;
            }
            break;
        case MacroActionKind::BookmarkConvertCase:
            if (const json::Value* v = value.find("mode"); v != nullptr) {
                const std::optional<domain::bookmarks::CaseMode> mode = caseModeFromString(v->stringValue);
                if (!mode.has_value()) {
                    if (diagnostic != nullptr) *diagnostic = "無法辨識的大小寫模式：" + v->stringValue;
                    return std::nullopt;
                }
                action.bookmarkCaseMode = *mode;
            }
            break;
        case MacroActionKind::BookmarkFindReplace:
            if (const json::Value* v = value.find("find"); v != nullptr) action.bookmarkFindReplace.find = v->stringValue;
            if (const json::Value* v = value.find("replace"); v != nullptr) action.bookmarkFindReplace.replace = v->stringValue;
            if (const json::Value* v = value.find("caseSensitive"); v != nullptr) {
                action.bookmarkFindReplace.caseSensitive = v->boolValue;
            }
            if (const json::Value* v = value.find("wholeWord"); v != nullptr) {
                action.bookmarkFindReplace.wholeWord = v->boolValue;
            }
            if (const json::Value* v = value.find("firstOccurrenceOnly"); v != nullptr) {
                action.bookmarkFindReplace.firstOccurrenceOnly = v->boolValue;
            }
            break;
        case MacroActionKind::BookmarkMergeDuplicates:
            if (const json::Value* v = value.find("caseSensitive"); v != nullptr) {
                action.bookmarkMergeDuplicates.caseSensitive = v->boolValue;
            }
            if (const json::Value* v = value.find("requireSameTarget"); v != nullptr) {
                action.bookmarkMergeDuplicates.requireSameTarget = v->boolValue;
            }
            if (const json::Value* v = value.find("mergeChildren"); v != nullptr) {
                action.bookmarkMergeDuplicates.mergeChildren = v->boolValue;
            }
            break;
    }
    return action;
}

[[nodiscard]] inline std::string serializeMacro(const MacroDefinition& macro) {
    json::Object root;
    root.emplace_back("name", json::Value::makeString(macro.name));
    json::Array steps;
    steps.reserve(macro.steps.size());
    for (const MacroAction& action : macro.steps) steps.push_back(actionToJson(action));
    root.emplace_back("steps", json::Value::makeArray(std::move(steps)));
    return json::serialize(json::Value::makeObject(std::move(root)));
}

struct MacroParseResult {
    bool ok{false};
    std::string diagnostic;
    MacroDefinition macro;
};

[[nodiscard]] inline MacroParseResult parseMacro(std::string_view text) {
    MacroParseResult result;
    std::string diagnostic;
    const std::optional<json::Value> root = json::parse(text, &diagnostic);
    if (!root.has_value()) {
        result.diagnostic = "JSON 解析失敗：" + diagnostic;
        return result;
    }
    if (root->kind != json::ValueKind::Object) {
        result.diagnostic = "巨集檔案的最外層必須是 JSON 物件";
        return result;
    }
    if (const json::Value* name = root->find("name"); name != nullptr) {
        result.macro.name = name->stringValue;
    }
    const json::Value* steps = root->find("steps");
    if (steps == nullptr || steps->kind != json::ValueKind::Array) {
        result.diagnostic = "巨集檔案缺少 \"steps\" 陣列";
        return result;
    }
    for (const json::Value& step : steps->arrayValue) {
        std::string stepDiagnostic;
        std::optional<MacroAction> action = actionFromJson(step, &stepDiagnostic);
        if (!action.has_value()) {
            result.diagnostic = stepDiagnostic;
            return result;
        }
        result.macro.steps.push_back(std::move(*action));
    }

    const std::string validation = result.macro.validate();
    if (!validation.empty()) {
        result.diagnostic = validation;
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::domain::macro
