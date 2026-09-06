#pragma once

// 表單計算圖（PRD-FORM-022 的另一半）。
//
// 單一運算式的安全性由 expression.h 負責；這一層負責的是**欄位之間的相依關係**。
// 那才是真正會爆掉的地方：A 的運算式參照 B、B 又參照 A，天真的實作會一路遞迴
// 到堆疊溢位。堆疊溢位在 Windows 上是無法攔截的 SEH，程式直接消失，
// 使用者看到的是「開檔就閃退」——而觸發它只需要一份精心構造的 PDF。
//
// 因此這裡的走訪是顯式的深度優先加造訪中集合：偵測到回邊就回報
// CircularReference 並附上完整的環路路徑，讓使用者知道該改哪一格。
//
// 與 expression.h 相同，這個檔案不連結 PDFium、不連結 Qt、不碰檔案系統。

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/formbuild/expression.h"

namespace alioth::engine::formbuild {

struct CalculationLimits {
    // 相依鏈的最大長度。與運算式自身的巢狀深度是兩回事：
    // 每一條運算式都可以很淺，但 A→B→C→… 的鏈可以很長。
    std::size_t maxDependencyDepth{32};

    // 參與計算的欄位總數。上限的意義是「解析一份文件的最壞成本有界」。
    std::size_t maxFields{4096};

    ExpressionLimits expression{};
};

struct FieldCalcResult {
    bool ok{false};
    double value{0.0};
    std::string text;  // 依 decimalPlaces 格式化後的字串，可直接寫回 /V
    ExpressionError error{ExpressionError::None};
    std::string detail;

    // 循環參照時的環路，依走訪順序且尾端重複起點（a → b → a）。
    std::vector<std::string> cycle;
};

// 求值時的輸出格式。表單欄位的值是字串，不格式化就會出現 0.30000000000000004。
struct CalcFormat {
    int decimalPlaces{2};
    bool trimTrailingZeros{true};
};

[[nodiscard]] std::string formatCalcValue(double value, const CalcFormat& format);

class FormCalculator {
public:
    explicit FormCalculator(CalculationLimits limits = {});

    // 註冊一個「有計算式」的欄位。運算式解析失敗時回傳 false，
    // 但仍會記住這個欄位，讓後續求值回報明確的解析錯誤而不是「找不到欄位」。
    bool setExpression(const std::string& fieldName, const std::string& expression);

    // 註冊一個「使用者輸入」的欄位。同名欄位若已有運算式，運算式優先。
    bool setValue(const std::string& fieldName, std::string value);

    void setFormat(const std::string& fieldName, CalcFormat format);

    void remove(const std::string& fieldName);
    void clear();

    [[nodiscard]] bool has(const std::string& fieldName) const;
    [[nodiscard]] std::size_t fieldCount() const noexcept { return order_.size(); }

    // 依註冊順序列出所有欄位名，讓 evaluateAll 的輸出順序可預期。
    [[nodiscard]] const std::vector<std::string>& fieldNames() const noexcept { return order_; }

    [[nodiscard]] FieldCalcResult evaluate(const std::string& fieldName) const;

    // 一次求出所有具運算式的欄位。單一欄位失敗不會中止其餘欄位——
    // 一格算不出來就整張表單空白，是使用者最難自行排查的失敗形態。
    [[nodiscard]] std::unordered_map<std::string, FieldCalcResult> evaluateAll() const;

    // 靜態檢查：不求值，只找出所有循環參照與參照不存在欄位的問題。
    // 用於「儲存前驗證」，讓錯誤在寫檔時就被擋下而不是等使用者填到那一格。
    struct Diagnosis {
        bool ok{true};
        std::vector<FieldCalcResult> problems;  // 每筆都帶欄位名於 detail
    };
    [[nodiscard]] Diagnosis diagnose() const;

private:
    struct Entry {
        Expression expression;
        std::string literal;
        CalcFormat format{};
        bool computed{false};
    };

    // 顯式的走訪狀態。用陣列而不是遞迴的區域變數，是為了讓「造訪中」這件事
    // 在整棵樹上是共享的——那是偵測回邊的唯一依據。
    struct Walk {
        std::unordered_set<std::string> visiting;
        std::vector<std::string> path;
        std::unordered_map<std::string, FieldCalcResult> done;
    };

    [[nodiscard]] FieldCalcResult resolve(const std::string& fieldName, Walk& walk,
                                          std::size_t depth) const;

    // 純結構走訪：不求值，只找環路。以顯式堆疊實作，理由見 SDD §7
    // （不可信任輸入決定的遞迴深度一律改成迭代並設上限）。
    [[nodiscard]] std::vector<std::string> findCycleFrom(const std::string& fieldName) const;

    CalculationLimits limits_;
    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> order_;
};

}  // namespace alioth::engine::formbuild
