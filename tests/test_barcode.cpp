// Code 128 條碼編碼（PRD-ENH-007 / PRD-FORM-025，WBS 批次處理）。
//
// 這支測試驗得到的只有三件事（見 domain/barcode.h 開頭的風險說明）：
// 校驗碼公式的算術正確性、寬度表本身的結構一致性、編碼後解碼能否還原。
// 它驗不到「這張表是否真的符合 ISO/IEC 15417」——那需要實體或軟體掃描器，
// 不屬於單元測試的範圍，已在 WP35 報告中列為交付前的必要步驟。

#include <QtTest>

#include "domain/barcode.h"

using namespace alioth::domain::barcode;

class TestBarcode : public QObject {
    Q_OBJECT

private slots:
    // 校驗碼公式可以獨立用算術驗證，不依賴寬度表對不對。
    void checksumMatchesHandComputedArithmetic() {
        // "A" 的符號值 = 'A'(65) - 0x20(32) = 33。
        // sum = START_B(104) + 33*1 = 137；137 % 103 = 34。
        const std::vector<int> values = {33};
        QCOMPARE(computeChecksumValue(104, values), 34);

        // 兩個字元 "AB"：33, 34。sum = 104 + 33*1 + 34*2 = 104+33+68=205；
        // 205 除以 103 商 1 餘 102，所以 205 % 103 = 102。
        const std::vector<int> values2 = {33, 34};
        QCOMPARE(computeChecksumValue(104, values2), 102);

        // 空字串（0 個資料符號）：sum = start，符合定義上的邊界情況。
        QCOMPARE(computeChecksumValue(104, {}), 104 % 103);
    }

    void patternTableIsStructurallyConsistent() {
        QVERIFY(patternTableStructurallyValid());
    }

    void emptyOrNonAsciiInputFailsExplicitly() {
        QVERIFY(!encodeCode128("").ok);
        QVERIFY(!encodeCode128("CJK\xE4\xB8\xAD").ok);  // 含非 ASCII 位元組
    }

    void encodesKnownSymbolValuesAndChecksum() {
        const Code128Result code = encodeCode128("A");
        QVERIFY2(code.ok, code.diagnostic.c_str());
        QCOMPARE(code.symbolValues.size(), std::size_t{1});
        QCOMPARE(code.symbolValues[0], 33);
        QCOMPARE(code.checksumValue, 34);
        // 起始碼 + 1 個資料符號 + 校驗碼 + 終止碼。
        QCOMPARE(code.symbolCount(), std::size_t{4});
    }

    void totalModulesMatchesSumOfRunWidths() {
        const Code128Result code = encodeCode128("Alioth-35");
        QVERIFY(code.ok);
        int sum = 0;
        for (const BarcodeRun& run : code.runs) sum += run.widthModules;
        QCOMPARE(sum, code.totalModules);
        // 每個資料符號 11 個模組，起始碼與校驗碼各 11，終止碼 13。
        const int expected =
            static_cast<int>(code.symbolValues.size() + 2) * 11 + 13;
        QCOMPARE(code.totalModules, expected);
    }

    void runsAlwaysStartWithABar() {
        const Code128Result code = encodeCode128("test123");
        QVERIFY(code.ok);
        QVERIFY(!code.runs.empty());
        QVERIFY(code.runs.front().bar);
    }

    // 編碼後解碼：這是唯一能在不依賴外部掃描器的情況下驗證「編碼器有沒有把
    // 自己的表用對」的方法。它不驗證表格是否符合標準，只驗證內部一致性。
    void decodingRoundTripsBackToSameSymbolValues() {
        const Code128Result code = encodeCode128("Roundtrip Test 42");
        QVERIFY(code.ok);
        const Code128Decoded decoded = decodeCode128(code.runs);
        QVERIFY2(decoded.ok, decoded.diagnostic.c_str());

        std::vector<int> expected;
        expected.push_back(104);  // START B
        for (const int v : code.symbolValues) expected.push_back(v);
        expected.push_back(code.checksumValue);
        QCOMPARE(decoded.symbolValues, expected);
    }

    void barsContentStreamOnlyEmitsForBarRuns() {
        const Code128Result code = encodeCode128("X");
        QVERIFY(code.ok);
        const alioth::domain::RectF rect{0.0, 0.0, 100.0, 20.0};
        const std::string content = barcodeBarsContentStream(code, rect);
        QVERIFY(!content.empty());
        // 至少要有跟 bar 數量一樣多的 "re" 指令。
        int barCount = 0;
        for (const BarcodeRun& run : code.runs) barCount += run.bar ? 1 : 0;
        int reCount = 0;
        std::size_t pos = 0;
        while ((pos = content.find(" re\n", pos)) != std::string::npos) { ++reCount; pos += 4; }
        QCOMPARE(reCount, barCount);
        QVERIFY(content.find("f\n") != std::string::npos);
    }

    void emptyRectProducesNoContent() {
        const Code128Result code = encodeCode128("X");
        QVERIFY(code.ok);
        QVERIFY(barcodeBarsContentStream(code, alioth::domain::RectF{}).empty());
    }
};

QTEST_MAIN(TestBarcode)
#include "test_barcode.moc"
