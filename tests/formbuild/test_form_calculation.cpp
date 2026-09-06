// 表單計算（PRD-FORM-022）。
//
// 這一層存在的理由是安全立場：PRD §2.1 排除 JavaScript 引擎，因為它與「關閉 V8
// 以縮小攻擊面」直接衝突。運算式來自使用者的 PDF，也就是不可信任輸入，
// 所以這組測試對「拒絕」的驗證比對「算對」的驗證更密集——
// 算錯是功能缺陷，算不完或吃光記憶體是安全缺陷。

#include <QtTest>

#include <string>

#include "engine/formbuild/form_calculation.h"

using namespace alioth::engine::formbuild;

namespace {

FormCalculator withFields() {
    FormCalculator calc;
    calc.setValue("price", "100");
    calc.setValue("quantity", "3");
    calc.setValue("discount", "0.1");
    return calc;
}

}  // namespace

class TestFormCalculation : public QObject {
    Q_OBJECT

private slots:
    void arithmeticOverFieldReferences() {
        FormCalculator calc = withFields();
        QVERIFY(calc.setExpression("total", "price * quantity"));

        const FieldCalcResult result = calc.evaluate("total");
        QVERIFY2(result.ok, result.detail.c_str());
        QCOMPARE(result.value, 300.0);
    }

    void chainedDependenciesResolveInOrder() {
        // 註冊順序與相依順序不一致是常態：使用者不會照拓樸序建欄位。
        FormCalculator calc = withFields();
        QVERIFY(calc.setExpression("grandTotal", "afterDiscount * 2"));
        QVERIFY(calc.setExpression("afterDiscount", "subtotal - subtotal * discount"));
        QVERIFY(calc.setExpression("subtotal", "price * quantity"));

        const FieldCalcResult result = calc.evaluate("grandTotal");
        QVERIFY2(result.ok, result.detail.c_str());
        QCOMPARE(result.value, 540.0);
    }

    void circularReferenceIsDetectedAndReported() {
        // 沒有這道檢查，一個互相參照的表單會讓求值無限遞迴。
        // 而且要回報環路本身——只說「有循環」使用者無從修起。
        FormCalculator calc;
        QVERIFY(calc.setExpression("a", "b + 1"));
        QVERIFY(calc.setExpression("b", "a + 1"));

        const FieldCalcResult result = calc.evaluate("a");
        QVERIFY(!result.ok);
        QVERIFY2(!result.cycle.empty(), "偵測到循環卻沒回報環路");
        // 環路尾端重複起點，讓呼叫端直接印成 a → b → a。
        QCOMPARE(result.cycle.front(), result.cycle.back());
    }

    void selfReferenceIsACycleToo() {
        FormCalculator calc;
        QVERIFY(calc.setExpression("x", "x + 1"));
        const FieldCalcResult result = calc.evaluate("x");
        QVERIFY(!result.ok);
        QVERIFY(!result.cycle.empty());
    }

    void dependencyDepthIsBounded() {
        // 每條運算式都很淺，但相依鏈可以很長——這是兩個不同的上限。
        CalculationLimits limits;
        limits.maxDependencyDepth = 4;
        FormCalculator calc{limits};

        calc.setValue("f0", "1");
        for (int i = 1; i <= 10; ++i) {
            QVERIFY(calc.setExpression("f" + std::to_string(i),
                                       "f" + std::to_string(i - 1) + " + 1"));
        }

        const FieldCalcResult deep = calc.evaluate("f10");
        QVERIFY2(!deep.ok, "超過相依深度上限卻仍然求值成功");
    }

    void fieldCountIsBounded() {
        CalculationLimits limits;
        limits.maxFields = 3;
        FormCalculator calc{limits};

        QVERIFY(calc.setValue("a", "1"));
        QVERIFY(calc.setValue("b", "2"));
        QVERIFY(calc.setValue("c", "3"));
        QVERIFY2(!calc.setValue("d", "4"), "超過欄位數上限卻仍然接受註冊");
    }

    void missingFieldIsAnExplicitError() {
        FormCalculator calc = withFields();
        QVERIFY(calc.setExpression("total", "price * nonexistent"));

        const FieldCalcResult result = calc.evaluate("total");
        QVERIFY(!result.ok);
        QVERIFY(!result.detail.empty());
    }

    void oneBadFieldDoesNotBlankTheWholeForm() {
        // 一格算不出來就整張表單空白，是使用者最難自行排查的失敗形態。
        FormCalculator calc = withFields();
        QVERIFY(calc.setExpression("good", "price + quantity"));
        calc.setExpression("bad", "price / 0");

        const auto all = calc.evaluateAll();
        QVERIFY(all.at("good").ok);
        QVERIFY(!all.at("bad").ok);
    }

    void diagnoseFindsProblemsWithoutEvaluating() {
        // 儲存前驗證：錯誤要在寫檔時被擋下，而不是等使用者填到那一格才發現。
        FormCalculator calc;
        calc.setExpression("a", "b + 1");
        calc.setExpression("b", "a + 1");
        calc.setExpression("c", "missing + 1");

        const FormCalculator::Diagnosis diagnosis = calc.diagnose();
        QVERIFY(!diagnosis.ok);
        QVERIFY(diagnosis.problems.size() >= 2);
    }

    void healthyFormDiagnosesClean() {
        FormCalculator calc = withFields();
        calc.setExpression("total", "price * quantity");
        const FormCalculator::Diagnosis diagnosis = calc.diagnose();
        QVERIFY2(diagnosis.ok, "正常的表單被診斷成有問題");
    }

    void valueFormattingAvoidsFloatingPointNoise() {
        // 表單欄位的值是字串。不格式化就會寫出 0.30000000000000004，
        // 而那會出現在使用者列印出來的合約上。
        CalcFormat format;
        format.decimalPlaces = 2;
        QCOMPARE(formatCalcValue(0.1 + 0.2, format), std::string{"0.3"});
        QCOMPARE(formatCalcValue(300.0, format), std::string{"300"});

        CalcFormat fixed;
        fixed.decimalPlaces = 2;
        fixed.trimTrailingZeros = false;
        QCOMPARE(formatCalcValue(300.0, fixed), std::string{"300.00"});
    }

    void removingAndClearingFields() {
        FormCalculator calc = withFields();
        QVERIFY(calc.has("price"));
        calc.remove("price");
        QVERIFY(!calc.has("price"));

        calc.clear();
        QCOMPARE(calc.fieldCount(), std::size_t{0});
    }

    void expressionOverridesUserValueForTheSameField() {
        // 同名欄位既有輸入又有運算式時，運算式優先——否則計算欄位會被
        // 使用者的舊輸入蓋掉，看起來像計算沒有生效。
        FormCalculator calc;
        calc.setValue("total", "999");
        QVERIFY(calc.setExpression("total", "1 + 1"));

        const FieldCalcResult result = calc.evaluate("total");
        QVERIFY(result.ok);
        QCOMPARE(result.value, 2.0);
    }
};

QTEST_APPLESS_MAIN(TestFormCalculation)
#include "test_form_calculation.moc"
