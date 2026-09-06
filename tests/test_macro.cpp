// 巨集系統的動作綱要與序列化（PRD-MAC-001，WBS 批次處理）。
//
// 這支測試驗的是「巨集是資料不是程式」這條安全立場的具體後果：
// 每個動作都能被序列化成 JSON、重新解析回相同的結構、且不合法的組合
// （角度不是 90 的倍數、條碼文字為空……）在驗證階段就被擋下，
// 不是等到真的去操作某個檔案才發現。

#include <QtTest>

#include "domain/macro.h"

using namespace alioth;
using namespace alioth::domain::macro;

class TestMacro : public QObject {
    Q_OBJECT

private slots:
    void roundTripsThroughJson() {
        MacroDefinition macro;
        macro.name = "Rotate then grayscale";

        MacroAction rotate;
        rotate.kind = MacroActionKind::RotatePages;
        rotate.pages = {0, 2};
        rotate.rotationDegrees = 90;
        macro.steps.push_back(rotate);

        MacroAction color;
        color.kind = MacroActionKind::ConvertColor;
        color.colorSettings.mode = domain::enhance::ColorTransformMode::Desaturate;
        color.colorSettings.desaturateAmount = 0.5;
        macro.steps.push_back(color);

        MacroAction barcode;
        barcode.kind = MacroActionKind::AddBarcodeStamp;
        barcode.barcodeText = "INV-0042";
        barcode.barcodeRect = alioth::domain::RectF{36.0, 36.0, 200.0, 80.0};
        macro.steps.push_back(barcode);

        QVERIFY(macro.validate().empty());

        const std::string json = serializeMacro(macro);
        const MacroParseResult parsed = parseMacro(json);
        QVERIFY2(parsed.ok, parsed.diagnostic.c_str());

        QCOMPARE(parsed.macro.name, macro.name);
        QCOMPARE(parsed.macro.steps.size(), std::size_t{3});

        QCOMPARE(parsed.macro.steps[0].kind, MacroActionKind::RotatePages);
        QCOMPARE(parsed.macro.steps[0].pages, (std::vector<std::int32_t>{0, 2}));
        QCOMPARE(parsed.macro.steps[0].rotationDegrees, 90);

        QCOMPARE(parsed.macro.steps[1].kind, MacroActionKind::ConvertColor);
        QCOMPARE(parsed.macro.steps[1].colorSettings.mode,
                 domain::enhance::ColorTransformMode::Desaturate);
        QCOMPARE(parsed.macro.steps[1].colorSettings.desaturateAmount, 0.5);

        QCOMPARE(parsed.macro.steps[2].kind, MacroActionKind::AddBarcodeStamp);
        QCOMPARE(parsed.macro.steps[2].barcodeText, std::string("INV-0042"));
        QCOMPARE(parsed.macro.steps[2].barcodeRect.left, 36.0);
        QCOMPARE(parsed.macro.steps[2].barcodeRect.top, 80.0);
    }

    void malformedJsonFailsExplicitly() {
        const MacroParseResult parsed = parseMacro("{ this is not json");
        QVERIFY(!parsed.ok);
        QVERIFY(!parsed.diagnostic.empty());
    }

    void missingStepsFieldFailsExplicitly() {
        const MacroParseResult parsed = parseMacro("{\"name\":\"x\"}");
        QVERIFY(!parsed.ok);
        QVERIFY(parsed.diagnostic.find("steps") != std::string::npos);
    }

    void unknownActionKindFailsExplicitly() {
        const MacroParseResult parsed =
            parseMacro("{\"name\":\"x\",\"steps\":[{\"action\":\"DeleteEverything\"}]}");
        QVERIFY(!parsed.ok);
    }

    void invalidRotationDegreesFailValidation() {
        MacroAction rotate;
        rotate.kind = MacroActionKind::RotatePages;
        rotate.rotationDegrees = 45;
        QVERIFY(!rotate.validate().empty());
    }

    void emptyBarcodeTextFailsValidation() {
        MacroAction barcode;
        barcode.kind = MacroActionKind::AddBarcodeStamp;
        barcode.barcodeRect = alioth::domain::RectF{0.0, 0.0, 100.0, 40.0};
        QVERIFY(!barcode.validate().empty());
    }

    void macroWithNoStepsFailsValidation() {
        MacroDefinition macro;
        macro.name = "empty";
        QVERIFY(!macro.validate().empty());
    }

    void emptyPagesArrayMeansAllPages() {
        const std::string json =
            "{\"name\":\"x\",\"steps\":[{\"action\":\"RotatePages\",\"pages\":[],"
            "\"rotationDegrees\":180}]}";
        const MacroParseResult parsed = parseMacro(json);
        QVERIFY2(parsed.ok, parsed.diagnostic.c_str());
        QVERIFY(parsed.macro.steps[0].pages.empty());
    }

    // PRD-BM-020：進階書籤巨集接進既有的巨集/序列化機制，不是另一套系統，
    // 所以測試重點與其餘動作一致——JSON 往返、驗證擋不合法輸入。
    // 樹狀邏輯本身（applyAffix／convertCase……）已經在 test_bookmark_ops
    // 測過，這裡不重複驗證。
    void bookmarkMacroActionsRoundTripThroughJson() {
        MacroDefinition macro;
        macro.name = "Tidy bookmarks";

        MacroAction affix;
        affix.kind = MacroActionKind::BookmarkAddAffix;
        affix.bookmarkAffix.prefix = "Ch. ";
        affix.bookmarkAffix.level = 0;
        macro.steps.push_back(affix);

        MacroAction everyN;
        everyN.kind = MacroActionKind::BookmarkEveryNPages;
        everyN.bookmarkInterval = 10;
        everyN.bookmarkFirstPage = 1;
        everyN.bookmarkTitlePattern = "Sheet {page}";
        macro.steps.push_back(everyN);

        MacroAction caseConv;
        caseConv.kind = MacroActionKind::BookmarkConvertCase;
        caseConv.bookmarkCaseMode = domain::bookmarks::CaseMode::Upper;
        macro.steps.push_back(caseConv);

        MacroAction findReplace;
        findReplace.kind = MacroActionKind::BookmarkFindReplace;
        findReplace.bookmarkFindReplace.find = "Draft";
        findReplace.bookmarkFindReplace.replace = "Final";
        macro.steps.push_back(findReplace);

        MacroAction merge;
        merge.kind = MacroActionKind::BookmarkMergeDuplicates;
        merge.bookmarkMergeDuplicates.requireSameTarget = false;
        macro.steps.push_back(merge);

        QVERIFY2(macro.validate().empty(), macro.validate().c_str());

        const std::string json = serializeMacro(macro);
        const MacroParseResult parsed = parseMacro(json);
        QVERIFY2(parsed.ok, parsed.diagnostic.c_str());
        QCOMPARE(parsed.macro.steps.size(), std::size_t{5});

        QCOMPARE(parsed.macro.steps[0].kind, MacroActionKind::BookmarkAddAffix);
        QCOMPARE(parsed.macro.steps[0].bookmarkAffix.prefix, std::string("Ch. "));
        QVERIFY(parsed.macro.steps[0].bookmarkAffix.level.has_value());
        QCOMPARE(*parsed.macro.steps[0].bookmarkAffix.level, 0);

        QCOMPARE(parsed.macro.steps[1].kind, MacroActionKind::BookmarkEveryNPages);
        QCOMPARE(parsed.macro.steps[1].bookmarkInterval, std::int32_t{10});
        QCOMPARE(parsed.macro.steps[1].bookmarkTitlePattern, std::string("Sheet {page}"));

        QCOMPARE(parsed.macro.steps[2].kind, MacroActionKind::BookmarkConvertCase);
        QCOMPARE(parsed.macro.steps[2].bookmarkCaseMode, domain::bookmarks::CaseMode::Upper);

        QCOMPARE(parsed.macro.steps[3].kind, MacroActionKind::BookmarkFindReplace);
        QCOMPARE(parsed.macro.steps[3].bookmarkFindReplace.find, std::string("Draft"));
        QCOMPARE(parsed.macro.steps[3].bookmarkFindReplace.replace, std::string("Final"));

        QCOMPARE(parsed.macro.steps[4].kind, MacroActionKind::BookmarkMergeDuplicates);
        QCOMPARE(parsed.macro.steps[4].bookmarkMergeDuplicates.requireSameTarget, false);
    }

    void bookmarkAddAffixWithoutPrefixOrSuffixFailsValidation() {
        MacroAction affix;
        affix.kind = MacroActionKind::BookmarkAddAffix;
        QVERIFY(!affix.validate().empty());
    }

    void bookmarkEveryNPagesWithZeroIntervalFailsValidation() {
        MacroAction everyN;
        everyN.kind = MacroActionKind::BookmarkEveryNPages;
        everyN.bookmarkInterval = 0;
        QVERIFY(!everyN.validate().empty());
    }

    void bookmarkFindReplaceWithEmptyFindFailsValidation() {
        MacroAction findReplace;
        findReplace.kind = MacroActionKind::BookmarkFindReplace;
        QVERIFY(!findReplace.validate().empty());
    }

    void jsonParserHandlesEscapedStrings() {
        using namespace alioth::domain::macro::json;
        std::string diagnostic;
        const std::optional<Value> value =
            parse("\"line1\\nline2\\t\\\"quoted\\\"\"", &diagnostic);
        QVERIFY2(value.has_value(), diagnostic.c_str());
        QCOMPARE(value->stringValue, std::string("line1\nline2\t\"quoted\""));
    }
};

QTEST_MAIN(TestMacro)
#include "test_macro.moc"
