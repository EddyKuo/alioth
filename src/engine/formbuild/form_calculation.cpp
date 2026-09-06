#include "engine/formbuild/form_calculation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace alioth::engine::formbuild {

namespace {

[[nodiscard]] FieldCalcResult calcFailure(ExpressionError error, std::string detail) {
    FieldCalcResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

// 依序把環路路徑接成「a → b → a」。訊息本身就是修正指引，
// 只回一句「偵測到循環參照」等於要使用者自己在整張表單上找。
[[nodiscard]] std::string describeCycle(const std::vector<std::string>& cycle) {
    std::string text;
    for (std::size_t i = 0; i < cycle.size(); ++i) {
        if (i != 0) text += " → ";
        text += cycle[i];
    }
    return text;
}

}  // namespace

std::string formatCalcValue(double value, const CalcFormat& format) {
    if (!std::isfinite(value)) return {};

    int places = format.decimalPlaces;
    if (places < 0) places = 0;
    if (places > 10) places = 10;

    // -0 會被格式化成 "-0.00"。表單上顯示負零沒有任何意義，只會被當成 bug 回報。
    if (value == 0.0) value = 0.0;

    char buffer[64] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
    if (written <= 0) return {};
    std::string text(buffer, static_cast<std::size_t>(written));

    if (format.trimTrailingZeros && text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text.empty() || text == "-") text = "0";
    return text;
}

FormCalculator::FormCalculator(CalculationLimits limits) : limits_(limits) {}

bool FormCalculator::setExpression(const std::string& fieldName, const std::string& expression) {
    if (fieldName.empty()) return false;
    if (!entries_.contains(fieldName)) {
        if (order_.size() >= limits_.maxFields) return false;
        order_.push_back(fieldName);
    }
    Entry& entry = entries_[fieldName];
    entry.expression = Expression::parse(expression, limits_.expression);
    entry.computed = true;
    return entry.expression.valid();
}

bool FormCalculator::setValue(const std::string& fieldName, std::string value) {
    if (fieldName.empty()) return false;
    if (!entries_.contains(fieldName)) {
        if (order_.size() >= limits_.maxFields) return false;
        order_.push_back(fieldName);
    }
    Entry& entry = entries_[fieldName];
    entry.literal = std::move(value);
    if (!entry.computed) entry.expression = Expression{};
    return true;
}

void FormCalculator::setFormat(const std::string& fieldName, CalcFormat format) {
    const auto it = entries_.find(fieldName);
    if (it == entries_.end()) return;
    it->second.format = format;
}

void FormCalculator::remove(const std::string& fieldName) {
    entries_.erase(fieldName);
    order_.erase(std::remove(order_.begin(), order_.end(), fieldName), order_.end());
}

void FormCalculator::clear() {
    entries_.clear();
    order_.clear();
}

bool FormCalculator::has(const std::string& fieldName) const {
    return entries_.contains(fieldName);
}

namespace {

// 把「解相依」接到運算式求值上。每個欄位參照都會回頭走一次 resolve，
// 因此環路偵測與深度上限對整棵相依樹都成立，而不只在單一運算式內成立。
class DependencySource final : public FieldValueSource {
public:
    using Resolver = std::function<FieldCalcResult(const std::string&)>;

    explicit DependencySource(Resolver resolver) : resolver_(std::move(resolver)) {}

    [[nodiscard]] std::optional<std::string> fieldValue(const std::string& name) const override {
        const FieldCalcResult result = resolver_(name);
        if (!result.ok) return std::nullopt;
        return result.text;
    }

    // 覆寫數字版：相依欄位的值本來就是 double，經過字串往返會損失精度，
    // 而連鎖計算的表單會把那點誤差累積成使用者對不上的總計。
    [[nodiscard]] FieldNumber fieldNumber(const std::string& name) const override {
        const FieldCalcResult result = resolver_(name);
        if (!result.ok) {
            // 相依欄位的失敗必須原封不動往上帶。若在這裡退化成 present=false，
            // 使用者看到的會是「找不到欄位」而不是真正的原因（例如除以零）。
            if (!failed_) {
                failed_ = true;
                failure_ = result;
            }
            return {};
        }
        FieldNumber number;
        number.present = true;
        number.numeric = true;
        number.value = result.value;
        number.raw = result.text;
        return number;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const FieldCalcResult& failure() const noexcept { return failure_; }

private:
    Resolver resolver_;
    mutable bool failed_{false};
    mutable FieldCalcResult failure_{};
};

}  // namespace

FieldCalcResult FormCalculator::resolve(const std::string& fieldName, Walk& walk,
                                        std::size_t depth) const {
    if (const auto cached = walk.done.find(fieldName); cached != walk.done.end()) {
        return cached->second;
    }

    if (depth > limits_.maxDependencyDepth) {
        return calcFailure(ExpressionError::DepthExceeded,
                           "欄位「" + fieldName + "」的相依鏈超過深度上限");
    }

    const auto it = entries_.find(fieldName);
    if (it == entries_.end()) {
        return calcFailure(ExpressionError::UnknownField, "找不到欄位：" + fieldName);
    }

    if (walk.visiting.contains(fieldName)) {
        // 回邊：從路徑上第一次出現這個名字的位置切下來，就是精確的環路。
        FieldCalcResult result =
            calcFailure(ExpressionError::CircularReference, {});
        const auto start = std::find(walk.path.begin(), walk.path.end(), fieldName);
        result.cycle.assign(start, walk.path.end());
        result.cycle.push_back(fieldName);
        result.detail = "欄位計算存在循環參照：" + describeCycle(result.cycle);
        return result;
    }

    const Entry& entry = it->second;
    if (!entry.computed) {
        const NumberParse parsed = parseFieldNumber(entry.literal);
        FieldCalcResult result;
        if (!parsed.ok) {
            result = calcFailure(ExpressionError::TypeMismatch,
                                 "欄位「" + fieldName + "」的內容不是數字：" + entry.literal);
        } else {
            result.ok = true;
            result.value = parsed.value;
            result.text = entry.literal;
        }
        walk.done.emplace(fieldName, result);
        return result;
    }

    if (!entry.expression.valid()) {
        FieldCalcResult result = calcFailure(
            entry.expression.error(),
            "欄位「" + fieldName + "」的運算式無法解析：" + entry.expression.diagnostic());
        walk.done.emplace(fieldName, result);
        return result;
    }

    walk.visiting.insert(fieldName);
    walk.path.push_back(fieldName);

    DependencySource source([&](const std::string& dependency) {
        return resolve(dependency, walk, depth + 1);
    });
    const EvalOutcome outcome = entry.expression.evaluate(source);

    walk.path.pop_back();
    walk.visiting.erase(fieldName);

    FieldCalcResult result;
    if (source.failed()) {
        result = source.failure();
        result.ok = false;
    } else if (!outcome.ok) {
        result.error = outcome.error;
        result.detail = "欄位「" + fieldName + "」計算失敗：" + outcome.detail;
    } else {
        result.ok = true;
        result.value = outcome.value;
        result.text = formatCalcValue(outcome.value, entry.format);
    }

    // 環路的結果不進快取：同一個欄位在不同的走訪路徑下會得到不同的環路描述，
    // 快取住其中一條會讓另一條的訊息指向無關的欄位。
    if (result.error != ExpressionError::CircularReference) {
        walk.done.emplace(fieldName, result);
    }
    return result;
}

FieldCalcResult FormCalculator::evaluate(const std::string& fieldName) const {
    Walk walk;
    return resolve(fieldName, walk, 0);
}

std::unordered_map<std::string, FieldCalcResult> FormCalculator::evaluateAll() const {
    std::unordered_map<std::string, FieldCalcResult> results;
    results.reserve(order_.size());
    for (const std::string& name : order_) {
        // 每個欄位重新起一次走訪：共用快取雖然更快，但環路的路徑描述會被
        // 前一次走訪的殘留污染，而那正是這個功能最需要說清楚的訊息。
        Walk walk;
        results.emplace(name, resolve(name, walk, 0));
    }
    return results;
}

FormCalculator::Diagnosis FormCalculator::diagnose() const {
    Diagnosis diagnosis;
    for (const std::string& name : order_) {
        const auto it = entries_.find(name);
        if (it == entries_.end() || !it->second.computed) continue;

        const Entry& entry = it->second;
        if (!entry.expression.valid()) {
            diagnosis.ok = false;
            diagnosis.problems.push_back(calcFailure(
                entry.expression.error(),
                "欄位「" + name + "」的運算式無法解析：" + entry.expression.diagnostic()));
            continue;
        }
        for (const std::string& dependency : entry.expression.referencedFields()) {
            if (!entries_.contains(dependency)) {
                diagnosis.ok = false;
                diagnosis.problems.push_back(calcFailure(
                    ExpressionError::UnknownField,
                    "欄位「" + name + "」參照了不存在的欄位：" + dependency));
            }
        }

        // 只走相依關係、不做算術：靜態檢查的目的是在使用者還沒填任何資料時
        // 就能指出結構性錯誤，此時求值必然因為欄位是空的而得到無意義的結果。
        if (const auto cycle = findCycleFrom(name); !cycle.empty()) {
            diagnosis.ok = false;
            FieldCalcResult problem =
                calcFailure(ExpressionError::CircularReference,
                            "欄位計算存在循環參照：" + describeCycle(cycle));
            problem.cycle = cycle;
            diagnosis.problems.push_back(std::move(problem));
        }
    }
    return diagnosis;
}

std::vector<std::string> FormCalculator::findCycleFrom(const std::string& fieldName) const {
    // 顯式堆疊而不是遞迴：這條路徑的輸入完全來自 PDF，
    // SDD §7 要求不可信任輸入決定的遞迴深度一律改成迭代並設上限。
    struct Frame {
        std::string name;
        std::size_t next{0};
    };

    std::vector<Frame> stack;
    std::unordered_set<std::string> onStack;
    std::unordered_set<std::string> finished;

    stack.push_back(Frame{fieldName, 0});
    onStack.insert(fieldName);

    while (!stack.empty()) {
        if (stack.size() > limits_.maxDependencyDepth) return {};

        Frame& frame = stack.back();
        const auto it = entries_.find(frame.name);
        const std::vector<std::string>* deps = nullptr;
        if (it != entries_.end() && it->second.computed && it->second.expression.valid()) {
            deps = &it->second.expression.referencedFields();
        }

        if (deps != nullptr && frame.next < deps->size()) {
            const std::string dependency = (*deps)[frame.next];
            ++frame.next;
            if (onStack.contains(dependency)) {
                std::vector<std::string> cycle;
                const auto start = std::find_if(
                    stack.begin(), stack.end(),
                    [&](const Frame& f) { return f.name == dependency; });
                for (auto f = start; f != stack.end(); ++f) cycle.push_back(f->name);
                cycle.push_back(dependency);
                return cycle;
            }
            if (finished.contains(dependency)) continue;
            stack.push_back(Frame{dependency, 0});
            onStack.insert(dependency);
            continue;
        }

        finished.insert(frame.name);
        onStack.erase(frame.name);
        stack.pop_back();
    }
    return {};
}

}  // namespace alioth::engine::formbuild
