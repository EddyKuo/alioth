// 受限運算式引擎的測試（PRD-FORM-022）。
//
// 這個檔案的測試密度刻意高於本工作包其他部分：運算式的輸入直接來自 PDF，
// 而 PDF 是不可信任輸入。PRD §2.1 用「排除 JavaScript 引擎」換來的安全保證，
// 只有在這個引擎真的沒有逃逸路徑時才成立，因此畸形輸入與資源上限
// 與正確性一樣是驗收條件而不是加分項。

#include <QTest>

#include <cmath>
#include <string>
#include <vector>

#include "engine/formbuild/expression.h"

using alioth::engine::formbuild::EvalOutcome;
using alioth::engine::formbuild::Expression;
using alioth::engine::formbuild::ExpressionError;
using alioth::engine::formbuild::ExpressionLimits;
using alioth::engine::formbuild::MapFieldValueSource;
using alioth::engine::formbuild::parseFieldNumber;

namespace {

[[nodiscard]] EvalOutcome evaluate(const std::string& source,
                                   const MapFieldValueSource& values = {},
                                   const ExpressionLimits& limits = {}) {
    return Expression::parse(source, limits).evaluate(values);
}

[[nodiscard]] MapFieldValueSource sample() {
    MapFieldValueSource values;
    values.set("a", "10");
    values.set("b", "4");
    values.set("c", "-2.5");
    values.set("empty", "");
    values.set("text", "hello");
    values.set("money", "1,234.50");
    values.set("有中文的欄位", "7");
    return values;
}

}  // namespace

class TestExpression : public QObject {
    Q_OBJECT

private slots:
    void arithmeticAndPrecedence();
    void parenthesesAndUnary();
    void decimalsAndGrouping();
    void fieldReferences();
    void aggregateFunctions();
    void roundFunction();
    void comparisonOperators();
    void ifBranches();
    void ifDoesNotEvaluateUnusedBranch();
    void divideByZero();
    void emptyFieldCountsAsZero();
    void unknownField();
    void typeMismatch();
    void syntaxErrors_data();
    void syntaxErrors();
    void unknownFunctionAndArity();
    void sourceTooLong();
    void nestingTooDeep();
    void tooManyFieldReferences();
    void tooManyTokensAndNodes();
    void malformedInputDoesNotCrash();
    void referencedFieldsAreDeduplicated();
    void numberParsingRules();
};

void TestExpression::arithmeticAndPrecedence() {
    QCOMPARE(evaluate("1+2").value, 3.0);
    QCOMPARE(evaluate("7-2").value, 5.0);
    QCOMPARE(evaluate("6*7").value, 42.0);
    QCOMPARE(evaluate("9/2").value, 4.5);

    // 乘除先於加減。若優先序寫錯，這條會得到 9 而不是 7。
    QCOMPARE(evaluate("1+2*3").value, 7.0);
    QCOMPARE(evaluate("2*3+1").value, 7.0);

    // 減法與除法左結合。寫成右結合會得到 8 與 4。
    QCOMPARE(evaluate("10-3-2").value, 5.0);
    QCOMPARE(evaluate("16/4/2").value, 2.0);
}

void TestExpression::parenthesesAndUnary() {
    QCOMPARE(evaluate("(1+2)*3").value, 9.0);
    QCOMPARE(evaluate("2*(3+(4-1))").value, 12.0);

    QCOMPARE(evaluate("-5").value, -5.0);
    QCOMPARE(evaluate("-(2+3)").value, -5.0);
    QCOMPARE(evaluate("3 - -2").value, 5.0);
    QCOMPARE(evaluate("--7").value, 7.0);
    QCOMPARE(evaluate("+4").value, 4.0);
}

void TestExpression::decimalsAndGrouping() {
    QVERIFY(std::abs(evaluate("0.1+0.2").value - 0.3) < 1e-9);
    QCOMPARE(evaluate(".5*4").value, 2.0);
    QCOMPARE(evaluate("2.50*2").value, 5.0);
}

void TestExpression::fieldReferences() {
    const MapFieldValueSource values = sample();
    QCOMPARE(evaluate("a+b", values).value, 14.0);
    QCOMPARE(evaluate("a*b-c", values).value, 42.5);

    // 中括號形式：欄位名可以含非 ASCII 與空白。
    QCOMPARE(evaluate("[有中文的欄位]*2", values).value, 14.0);
}

void TestExpression::aggregateFunctions() {
    const MapFieldValueSource values = sample();
    QCOMPARE(evaluate("SUM(a, b, 1)", values).value, 15.0);
    QCOMPARE(evaluate("AVG(a, b)", values).value, 7.0);
    QCOMPARE(evaluate("MIN(a, b, c)", values).value, -2.5);
    QCOMPARE(evaluate("MAX(a, b, c)", values).value, 10.0);

    // 函式名大小寫不敏感；欄位名敏感。
    QCOMPARE(evaluate("sum(a, b)", values).value, 14.0);
    QCOMPARE(evaluate("Sum(a, b)", values).value, 14.0);

    // 巢狀與運算混用。
    QCOMPARE(evaluate("SUM(a, b) / 2 + MIN(1, 2)", values).value, 8.0);
}

void TestExpression::roundFunction() {
    QCOMPARE(evaluate("ROUND(2.4)").value, 2.0);
    QCOMPARE(evaluate("ROUND(2.5)").value, 3.0);
    QCOMPARE(evaluate("ROUND(-2.5)").value, -3.0);
    QCOMPARE(evaluate("ROUND(1.2345, 2)").value, 1.23);
    QCOMPARE(evaluate("ROUND(1.2355, 2)").value, 1.24);

    // 小數位數必須是 0..10 的整數，否則明確失敗而不是靜默取整。
    const EvalOutcome negative = evaluate("ROUND(1.5, -1)");
    QVERIFY(!negative.ok);
    QCOMPARE(negative.error, ExpressionError::ArityMismatch);
    QVERIFY(!evaluate("ROUND(1.5, 99)").ok);
    QVERIFY(!evaluate("ROUND(1.5, 1.5)").ok);
}

void TestExpression::comparisonOperators() {
    QCOMPARE(evaluate("1 < 2").value, 1.0);
    QCOMPARE(evaluate("2 < 1").value, 0.0);
    QCOMPARE(evaluate("2 <= 2").value, 1.0);
    QCOMPARE(evaluate("3 > 2").value, 1.0);
    QCOMPARE(evaluate("3 >= 4").value, 0.0);
    QCOMPARE(evaluate("2 == 2").value, 1.0);
    QCOMPARE(evaluate("2 = 2").value, 1.0);
    QCOMPARE(evaluate("2 != 3").value, 1.0);

    // 比較的優先序低於算術：1+1 == 2 應為真，而不是 1 + (1 == 2)。
    QCOMPARE(evaluate("1+1 == 2").value, 1.0);

    // 連續比較一律是語法錯誤，理由見 parseComparison 的註解。
    const Expression chained = Expression::parse("1 < 2 < 3");
    QVERIFY(!chained.valid());
    QCOMPARE(chained.error(), ExpressionError::SyntaxError);
}

void TestExpression::ifBranches() {
    MapFieldValueSource values;
    values.set("qty", "5");

    QCOMPARE(evaluate("IF(qty > 3, 100, 200)", values).value, 100.0);

    values.set("qty", "1");
    QCOMPARE(evaluate("IF(qty > 3, 100, 200)", values).value, 200.0);

    // 條件為 0 走假分支、非 0 走真分支。
    QCOMPARE(evaluate("IF(0, 1, 2)").value, 2.0);
    QCOMPARE(evaluate("IF(0.5, 1, 2)").value, 1.0);
    QCOMPARE(evaluate("IF(-1, 1, 2)").value, 1.0);
}

void TestExpression::ifDoesNotEvaluateUnusedBranch() {
    MapFieldValueSource values;
    values.set("qty", "0");
    values.set("total", "100");

    // 這是表單上最常見的防呆寫法。若 IF 先算完兩個分支才選，
    // 它會因為分母是零而失敗——而使用者寫它的目的正是避免這件事。
    const EvalOutcome guarded = evaluate("IF(qty == 0, 0, total / qty)", values);
    QVERIFY2(guarded.ok, guarded.detail.c_str());
    QCOMPARE(guarded.value, 0.0);

    // 未選中的分支參照不存在的欄位也不該失敗。
    const EvalOutcome lazy = evaluate("IF(1, 42, nosuchfield)", values);
    QVERIFY2(lazy.ok, lazy.detail.c_str());
    QCOMPARE(lazy.value, 42.0);
}

void TestExpression::divideByZero() {
    const EvalOutcome literal = evaluate("1/0");
    QVERIFY(!literal.ok);
    QCOMPARE(literal.error, ExpressionError::DivideByZero);

    MapFieldValueSource values;
    values.set("z", "0");
    values.set("n", "5");
    const EvalOutcome viaField = evaluate("n/z", values);
    QVERIFY(!viaField.ok);
    QCOMPARE(viaField.error, ExpressionError::DivideByZero);

    // 空欄位當成 0，因此拿它當分母同樣是除以零而不是型別錯誤。
    values.set("blank", "");
    const EvalOutcome viaEmpty = evaluate("n/blank", values);
    QVERIFY(!viaEmpty.ok);
    QCOMPARE(viaEmpty.error, ExpressionError::DivideByZero);
}

void TestExpression::emptyFieldCountsAsZero() {
    const MapFieldValueSource values = sample();
    QCOMPARE(evaluate("empty", values).value, 0.0);
    QCOMPARE(evaluate("a + empty", values).value, 10.0);

    // AVG 的分母是引數個數，空欄位一起算。與 Acrobat 一致；
    // 不一致的話同一份表單在兩邊會顯示不同的平均值。
    QCOMPARE(evaluate("AVG(a, empty)", values).value, 5.0);
    QCOMPARE(evaluate("MIN(a, empty)", values).value, 0.0);
}

void TestExpression::unknownField() {
    const MapFieldValueSource values = sample();
    const EvalOutcome outcome = evaluate("a + missing", values);
    QVERIFY(!outcome.ok);
    QCOMPARE(outcome.error, ExpressionError::UnknownField);
    // 訊息必須指出是哪個欄位，否則使用者得自己在整條式子裡找。
    QVERIFY(outcome.detail.find("missing") != std::string::npos);

    const EvalOutcome bracketed = evaluate("[不存在的欄位] + 1", values);
    QVERIFY(!bracketed.ok);
    QCOMPARE(bracketed.error, ExpressionError::UnknownField);
}

void TestExpression::typeMismatch() {
    const MapFieldValueSource values = sample();
    const EvalOutcome outcome = evaluate("a + text", values);
    QVERIFY(!outcome.ok);
    QCOMPARE(outcome.error, ExpressionError::TypeMismatch);
    QVERIFY(outcome.detail.find("text") != std::string::npos);

    // 千分位逗號是合法的，不該被判成型別錯誤。
    const EvalOutcome grouped = evaluate("money * 2", values);
    QVERIFY2(grouped.ok, grouped.detail.c_str());
    QCOMPARE(grouped.value, 2469.0);
}

void TestExpression::syntaxErrors_data() {
    QTest::addColumn<QString>("source");
    QTest::newRow("空字串") << "";
    QTest::newRow("只有空白") << "   \t\n";
    QTest::newRow("懸空運算子") << "1 +";
    QTest::newRow("開頭運算子") << "* 2";
    QTest::newRow("括號未收") << "(1 + 2";
    QTest::newRow("多餘右括號") << "1 + 2)";
    QTest::newRow("空括號") << "()";
    QTest::newRow("連續逗號") << "SUM(1,,2)";
    QTest::newRow("中括號未收") << "[abc";
    QTest::newRow("空中括號") << "[]";
    QTest::newRow("驚嘆號單獨") << "1 ! 2";
    QTest::newRow("不明字元") << "1 @ 2";
    QTest::newRow("跳脫嘗試") << "1; DROP TABLE x";
    QTest::newRow("尾端多餘") << "1 2";
}

void TestExpression::syntaxErrors() {
    QFETCH(QString, source);
    const Expression expression = Expression::parse(source.toStdString());
    QVERIFY2(!expression.valid(), source.toUtf8().constData());
    QVERIFY(!expression.diagnostic().empty());

    // 無效的運算式求值時也必須回報同一個錯誤，不得回傳「成功但值是 0」。
    MapFieldValueSource values;
    const EvalOutcome outcome = expression.evaluate(values);
    QVERIFY(!outcome.ok);
    QCOMPARE(outcome.error, expression.error());
}

void TestExpression::unknownFunctionAndArity() {
    // 白名單以外的名字加括號一律是「不支援的函式」，而不是被當成欄位。
    // 這一條是安全邊界：只要有任何一條路徑能讓未知名稱被當成可呼叫的東西，
    // 之後新增函式時就可能意外開放。
    for (const char* source : {"EVAL(1)", "REQUIRE(1)", "alert(1)", "print(1)", "SUMX(1)"}) {
        const Expression expression = Expression::parse(source);
        QVERIFY2(!expression.valid(), source);
        QCOMPARE(expression.error(), ExpressionError::UnknownFunction);
    }

    for (const char* source : {"IF(1, 2)", "IF(1, 2, 3, 4)", "ROUND()", "SUM()", "ROUND(1, 2, 3)"}) {
        const Expression expression = Expression::parse(source);
        QVERIFY2(!expression.valid(), source);
        QVERIFY(expression.error() == ExpressionError::ArityMismatch ||
                expression.error() == ExpressionError::SyntaxError);
    }
}

void TestExpression::sourceTooLong() {
    ExpressionLimits limits;
    limits.maxSourceLength = 32;

    const std::string longSource(64, '1');
    const Expression expression = Expression::parse(longSource, limits);
    QVERIFY(!expression.valid());
    QCOMPARE(expression.error(), ExpressionError::SourceTooLong);

    // 預設上限之下，剛好等於上限的輸入必須通過——上限是「超過才拒絕」。
    const Expression atLimit = Expression::parse(std::string(32, '1'), limits);
    QVERIFY(atLimit.valid());
}

void TestExpression::nestingTooDeep() {
    ExpressionLimits limits;
    limits.maxDepth = 8;

    const auto nested = [](std::size_t depth) {
        return std::string(depth, '(') + "1" + std::string(depth, ')');
    };

    QVERIFY(Expression::parse(nested(8), limits).valid());

    const Expression tooDeep = Expression::parse(nested(9), limits);
    QVERIFY(!tooDeep.valid());
    QCOMPARE(tooDeep.error(), ExpressionError::DepthExceeded);

    // 一元運算的連續套用也受同一個深度上限節制，否則 "-----...-1" 就是
    // 一條繞過括號限制的路徑。
    const Expression manyNegations = Expression::parse(std::string(64, '-') + "1", limits);
    QVERIFY(!manyNegations.valid());
    QCOMPARE(manyNegations.error(), ExpressionError::DepthExceeded);

    // 預設上限下的深層輸入也必須是明確失敗而不是堆疊溢位。
    const Expression defaultLimits = Expression::parse(nested(5000));
    QVERIFY(!defaultLimits.valid());
}

void TestExpression::tooManyFieldReferences() {
    ExpressionLimits limits;
    limits.maxFieldReferences = 4;
    limits.maxCallArguments = 64;

    const Expression ok = Expression::parse("SUM(a, b, c, d)", limits);
    QVERIFY(ok.valid());

    const Expression tooMany = Expression::parse("SUM(a, b, c, d, e)", limits);
    QVERIFY(!tooMany.valid());
    QCOMPARE(tooMany.error(), ExpressionError::TooManyFieldReferences);

    // 重複參照同一個欄位也計入總數：限制的是求值成本，不是不同名稱的個數。
    const Expression repeated = Expression::parse("a+a+a+a+a", limits);
    QVERIFY(!repeated.valid());
    QCOMPARE(repeated.error(), ExpressionError::TooManyFieldReferences);
}

void TestExpression::tooManyTokensAndNodes() {
    {
        ExpressionLimits limits;
        limits.maxTokens = 8;
        const Expression expression = Expression::parse("1+1+1+1+1+1+1+1+1+1", limits);
        QVERIFY(!expression.valid());
        QCOMPARE(expression.error(), ExpressionError::TooManyTokens);
    }
    {
        ExpressionLimits limits;
        limits.maxNodes = 5;
        const Expression expression = Expression::parse("1+2+3+4+5+6+7+8", limits);
        QVERIFY(!expression.valid());
        QCOMPARE(expression.error(), ExpressionError::TooManyNodes);
    }
    {
        ExpressionLimits limits;
        limits.maxCallArguments = 3;
        const Expression expression = Expression::parse("SUM(1,2,3,4,5)", limits);
        QVERIFY(!expression.valid());
        QCOMPARE(expression.error(), ExpressionError::TooManyArguments);
    }
    {
        ExpressionLimits limits;
        limits.maxFieldNameLength = 4;
        QVERIFY(!Expression::parse("[abcdefgh]", limits).valid());
        QVERIFY(!Expression::parse("abcdefgh", limits).valid());
    }
}

void TestExpression::malformedInputDoesNotCrash() {
    // 畸形輸入不得造成崩潰或無限迴圈。這裡不驗「應該回傳什麼」——
    // 有些輸入碰巧是合法的——只驗「一定會在有限時間內回傳，且不會當掉」。
    const std::vector<std::string> hostile = {
        "((((((((((((((((((((((((((((((((((((((((((((((((((",
        "))))))))))))))))))))",
        "[[[[[[[[[[[[[[[[[[[[",
        "]]]]]]]]]]]]]]]]]]]]",
        "[[a]]",
        "SUM(SUM(SUM(SUM(SUM(SUM(SUM(SUM(1))))))))",
        "IF(IF(IF(1,1,1),1,1),1,1)",
        std::string(900, '-') + "1",
        std::string(900, '+') + "1",
        std::string(400, '('),
        std::string(500, ','),
        std::string(300, '.'),
        "1........2",
        "1e999999",
        "999999999999999999999999999999999*999999999999999999999999999999999",
        "0/0",
        "-0/0",
        "SUM(,)",
        "SUM(1,)",
        ",,,,",
        "()()()",
        "a[b]c",
        "\x01\x02\x03\x04",
        "\xFF\xFE\xFD",
        std::string("abc\0def", 7),
        "/*comment*/1",
        "1//2",
        "#include <x>",
        "${a}",
        "a.b.c.d.e.f.g",
        "....",
        "[]",
        "[ ]",
        "[\n]",
        "IF",
        "IF(",
        "SUM(a",
        "((1)",
        "(1))",
        "1 1 1 1 1",
        "= = =",
        "<<<>>>",
        "!!!",
        "!",
        "1!=",
        "%",
        "^2",
        "~1",
        "&&",
        "||",
    };

    MapFieldValueSource values;
    values.set("a", "1");
    values.set("b", "2");
    values.set("c", "3");

    for (const std::string& source : hostile) {
        const Expression expression = Expression::parse(source);
        const EvalOutcome outcome = expression.evaluate(values);
        // 唯一的硬性要求：成功時值必須是有限數。失敗時只要有錯誤碼即可。
        if (outcome.ok) {
            QVERIFY2(std::isfinite(outcome.value), source.c_str());
        } else {
            QVERIFY2(outcome.error != ExpressionError::None, source.c_str());
        }
    }
}

void TestExpression::referencedFieldsAreDeduplicated() {
    const Expression expression = Expression::parse("a + b * a - [c d]");
    QVERIFY(expression.valid());
    const std::vector<std::string> expected{"a", "b", "c d"};
    QCOMPARE(expression.referencedFields(), expected);

    // 解析失敗時不得留下半套的參照清單：相依圖會照著它建出不存在的邊。
    const Expression broken = Expression::parse("a + b *");
    QVERIFY(!broken.valid());
    QVERIFY(broken.referencedFields().empty());
}

void TestExpression::numberParsingRules() {
    QVERIFY(parseFieldNumber("12").ok);
    QCOMPARE(parseFieldNumber("12").value, 12.0);
    QCOMPARE(parseFieldNumber(" -3.5 ").value, -3.5);
    QCOMPARE(parseFieldNumber("1,234").value, 1234.0);
    QCOMPARE(parseFieldNumber("+7").value, 7.0);

    QVERIFY(parseFieldNumber("").empty);
    QVERIFY(parseFieldNumber("   ").empty);
    QCOMPARE(parseFieldNumber("").value, 0.0);

    // 前綴解析（JavaScript 的 parseFloat 語意）必須被拒絕：
    // 打錯的資料被靜默算成看似合理的數字，比報錯危險得多。
    QVERIFY(!parseFieldNumber("12abc").ok);
    QVERIFY(!parseFieldNumber("abc").ok);
    QVERIFY(!parseFieldNumber("1.2.3").ok);
    QVERIFY(!parseFieldNumber("1e5").ok);
    QVERIFY(!parseFieldNumber("-").ok);
    QVERIFY(!parseFieldNumber("NaN").ok);
    QVERIFY(!parseFieldNumber("inf").ok);
}

QTEST_APPLESS_MAIN(TestExpression)
#include "test_expression.moc"
