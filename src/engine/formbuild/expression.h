#pragma once

// 受限運算式引擎（PRD-FORM-022、PRD §2.1 第 5 項、PRD §8.2「零腳本執行」）。
//
// PRD 明確把 JavaScript 引擎列為排除項，理由是與「PDFium 關閉 V8 以縮小攻擊面」
// 的安全立場直接衝突。表單計算因此改由這個自建引擎承擔。
//
// 設計目標不是「盡量像 JavaScript」，而是**讓引擎的能力上限等於攻擊者的能力上限**。
// 運算式來自 PDF 檔案，而 PDF 是不可信任輸入（SDD §7）；因此這裡刻意沒有：
//
//   - 變數指派與可變狀態：求值是純函數，同輸入必得同輸出
//   - 迴圈與跳躍：文法本身無法表達重複執行，終止性由文法保證而不是靠逾時
//   - 使用者自定函式與遞迴呼叫：函式表是編譯期固定的白名單
//   - 任何 I/O、時間、亂數、環境存取
//
// 少掉這些之後，唯一還能被濫用的資源是解析與求值的堆疊與節點數，
// 所以那三者都有明確上限（ExpressionLimits），超過即明確失敗而不是慢慢跑。
//
// 這個檔案不連結 PDFium、不連結 Qt，也不碰檔案系統。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alioth::engine::formbuild {

// 資源上限。全部給預設值，呼叫端通常不需要調整；
// 開放出來是為了讓測試能用很小的上限驗證「超過就失敗」而不必造出巨大的輸入。
struct ExpressionLimits {
    // 原始字串長度。先擋在這裡，後面的所有上限就都有了天然的上界。
    std::size_t maxSourceLength{1024};

    // 詞法單元數量。與長度分開限制，因為「(((((...」這種輸入每個字元都是一個單元。
    std::size_t maxTokens{512};

    // AST 節點數。限制的是解析結果的規模，與輸入長度不是同一件事
    // （函式呼叫一個字元可以展開成多個節點）。
    std::size_t maxNodes{512};

    // 遞迴下降的巢狀深度，同時也是求值遞迴的深度上限。
    // 這是唯一會直接吃到原生堆疊的資源，所以設得比其他項保守。
    std::size_t maxDepth{32};

    // 單一運算式可出現的欄位參照總數（含重複）。
    std::size_t maxFieldReferences{64};

    // 單一函式呼叫的引數個數。SUM 可以很長，但不該無限長。
    std::size_t maxCallArguments{32};

    // 欄位名稱長度。PDF 的欄位名沒有規格上限，但沒有上限就等於沒有防線。
    std::size_t maxFieldNameLength{128};
};

enum class ExpressionError : std::uint8_t {
    None,
    EmptySource,          // 空字串或只有空白
    SourceTooLong,
    TooManyTokens,
    TooManyNodes,
    DepthExceeded,
    TooManyFieldReferences,
    TooManyArguments,
    FieldNameTooLong,
    SyntaxError,
    UnknownFunction,
    ArityMismatch,
    UnknownField,         // 求值期：參照了不存在的欄位
    TypeMismatch,         // 求值期：欄位內容不是數字
    DivideByZero,
    NotFinite,            // 溢位或 0/0 之類產生的 inf / nan
    CircularReference,    // 由 FormCalculator 回填，運算式本身偵測不到
};

[[nodiscard]] const char* describe(ExpressionError error) noexcept;

// 欄位取值的三種結局。合成一個結構而不是用 optional 疊 optional，
// 是因為「不存在」「存在但不是數字」「存在且是數字」在表單上是三種不同的
// 使用者錯誤，訊息也不同；用 bool 組合表達遲早會有人只判其中兩種。
struct FieldNumber {
    bool present{false};
    bool numeric{false};
    double value{0.0};
    std::string raw;  // 型別不符時要能把原始內容顯示給使用者看
};

// 欄位值來源。回傳 nullopt 代表「欄位不存在」，回傳空字串代表「欄位存在但未填」——
// 兩者的求值行為不同（前者是錯誤、後者視為 0），混為一談會讓使用者無法分辨
// 是拼錯欄位名還是真的沒填。
class FieldValueSource {
public:
    virtual ~FieldValueSource();
    [[nodiscard]] virtual std::optional<std::string> fieldValue(const std::string& name) const = 0;

    // 求值實際走的是這個。預設實作由 fieldValue 推導，因此一般來源只要實作字串版。
    //
    // 開放覆寫是給「值本身已經是 double」的來源用的（例如計算欄位參照另一個
    // 計算欄位）：那種情況下經過字串往返會靜默損失精度，
    // 在連鎖計算的表單上累積成使用者對不上的總計。
    [[nodiscard]] virtual FieldNumber fieldNumber(const std::string& name) const;
};

// 以名稱／值對照表實作的來源，供測試與簡單情境使用。
class MapFieldValueSource final : public FieldValueSource {
public:
    void set(std::string name, std::string value);
    void erase(const std::string& name);
    void clear();

    [[nodiscard]] std::optional<std::string> fieldValue(const std::string& name) const override;

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

// 把欄位的字串值轉成數字。
//
// 規則刻意寫死且狹窄：去頭尾空白、移除千分位逗號、其餘必須是完整的十進位數字。
// 不接受 "12abc" 這種前綴解析——JavaScript 的 parseFloat 語意在表單上會把
// 打錯的資料靜默算成一個看似合理的數字，那比明確報錯危險得多。
struct NumberParse {
    bool ok{false};
    bool empty{false};  // 欄位存在但沒有內容；呼叫端一律視為 0
    double value{0.0};
};

[[nodiscard]] NumberParse parseFieldNumber(std::string_view text);

struct EvalOutcome {
    bool ok{false};
    double value{0.0};
    ExpressionError error{ExpressionError::None};
    std::string detail;  // 繁體中文，可直接顯示；例如出問題的欄位名
};

// 已解析的運算式。解析與求值分離的理由有兩個：
// 一是同一條運算式在表單上會被反覆求值，不該每次重新剖析；
// 二是相依圖需要「這條運算式參照了哪些欄位」，那只有 AST 知道。
class Expression {
public:
    Expression() = default;

    [[nodiscard]] static Expression parse(std::string_view source,
                                          const ExpressionLimits& limits = {});

    [[nodiscard]] bool valid() const noexcept { return error_ == ExpressionError::None && root_ >= 0; }
    [[nodiscard]] ExpressionError error() const noexcept { return error_; }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const std::string& source() const noexcept { return source_; }

    // 去重後的欄位參照，維持首次出現順序（讓相依圖的走訪順序可預期）。
    [[nodiscard]] const std::vector<std::string>& referencedFields() const noexcept {
        return referencedFields_;
    }

    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }

    [[nodiscard]] EvalOutcome evaluate(const FieldValueSource& values) const;

private:
    enum class NodeKind : std::uint8_t { Number, Field, Negate, Binary, Call };

    enum class BinaryOp : std::uint8_t { Add, Sub, Mul, Div, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

    enum class Function : std::uint8_t { Sum, Avg, Min, Max, Round, If };

    struct Node {
        NodeKind kind{NodeKind::Number};
        BinaryOp binary{BinaryOp::Add};
        Function function{Function::Sum};
        double number{0.0};
        std::string name;            // 欄位名
        std::vector<int> children;
    };

    friend class ExpressionParser;

    [[nodiscard]] EvalOutcome evaluateNode(int index, const FieldValueSource& values,
                                           std::size_t depth) const;
    [[nodiscard]] EvalOutcome evaluateCall(const Node& node, const FieldValueSource& values,
                                           std::size_t depth) const;

    std::string source_;
    std::vector<Node> nodes_;
    std::vector<std::string> referencedFields_;
    ExpressionLimits limits_{};
    int root_{-1};
    ExpressionError error_{ExpressionError::EmptySource};
    std::string diagnostic_{"運算式為空"};
};

}  // namespace alioth::engine::formbuild
