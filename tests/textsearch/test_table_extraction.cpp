// 矩形選取複製為 TSV 的測試（PRD-TXT-003）。純領域層，不載入 PDFium——
// 座標分群演算法的正確性最需要密集測試，而合成的字元外框可以精確控制
// 邊界情形（欄位缺格、跨欄合併、數字右對齊），這是領域層存在的理由。

#include <QtTest>

#include <string>
#include <vector>

#include "domain/table_extraction.h"
#include "domain/text_layer.h"

using namespace alioth::domain;

namespace {

// 每個字元寬 5 點、字高 12 點（bottom..top）。x 從 left 開始逐字往右排，
// 字元之間緊貼（gap = 0），保證同一詞的字元一定會被 tokenizeRow 合併
// （gap 門檻是字高的 0.35 倍 = 4.2 點，遠大於 0）。
void appendWord(std::vector<TextChar>& chars, std::int32_t& index, const std::string& text,
                double left, double bottom, double top, double charWidth = 5.0) {
    double x = left;
    for (const char c : text) {
        TextChar ch;
        ch.index = index++;
        ch.unicode = static_cast<char32_t>(static_cast<unsigned char>(c));
        ch.box = RectF{x, bottom, x + charWidth, top};
        chars.push_back(ch);
        x += charWidth;
    }
}

constexpr RectF kWholeArea{-100.0, -100.0, 400.0, 400.0};

}  // namespace

class TestTableExtraction : public QObject {
    Q_OBJECT

private slots:
    // (a) 對齊良好的表格：3 欄 2 列，欄與欄之間有明顯空白。
    void wellAlignedTable() {
        std::vector<TextChar> chars;
        std::int32_t index = 0;
        appendWord(chars, index, "Name", 0.0, 88.0, 100.0);
        appendWord(chars, index, "Age", 30.0, 88.0, 100.0);
        appendWord(chars, index, "City", 60.0, 88.0, 100.0);
        appendWord(chars, index, "Bob", 0.0, 68.0, 80.0);
        appendWord(chars, index, "30", 30.0, 68.0, 80.0);
        appendWord(chars, index, "NYC", 60.0, 68.0, 80.0);

        PageTextLayer layer{0, chars};
        const ExtractedTable table = extractTable(layer, kWholeArea);

        QCOMPARE(table.columnBoundaries.size(), std::size_t{3});
        QCOMPARE(table.rows.size(), std::size_t{2});
        QVERIFY(!table.ambiguous);
        QCOMPARE(table.rows[0], (std::vector<std::string>{"Name", "Age", "City"}));
        QCOMPARE(table.rows[1], (std::vector<std::string>{"Bob", "30", "NYC"}));

        const std::string tsv = tableToTsv(table);
        QCOMPARE(QString::fromStdString(tsv), QString("Name\tAge\tCity\nBob\t30\tNYC\n"));
    }

    // (b) 欄位缺格：第二列的「Age」欄沒有任何字元，輸出必須是空欄位（保留 Tab）
    // 而不是把後面的欄往前擠——貼到 Excel 時欄位要能對得上。
    void missingCellProducesEmptyField() {
        std::vector<TextChar> chars;
        std::int32_t index = 0;
        appendWord(chars, index, "Name", 0.0, 88.0, 100.0);
        appendWord(chars, index, "Age", 30.0, 88.0, 100.0);
        appendWord(chars, index, "City", 60.0, 88.0, 100.0);
        appendWord(chars, index, "Bob", 0.0, 68.0, 80.0);
        // 第二列故意不放 Age 欄的字元。
        appendWord(chars, index, "NYC", 60.0, 68.0, 80.0);

        PageTextLayer layer{0, chars};
        const ExtractedTable table = extractTable(layer, kWholeArea);

        QCOMPARE(table.columnBoundaries.size(), std::size_t{3});
        QCOMPARE(table.rows[1], (std::vector<std::string>{"Bob", "", "NYC"}));
        QCOMPARE(QString::fromStdString(tableToTsv(table)),
                 QString("Name\tAge\tCity\nBob\t\tNYC\n"));
    }

    // (c) 跨欄合併儲存格：這是本演算法明確做不到完美還原的情形（見
    // table_extraction.h 檔頭「跨欄合併儲存格」的說明）。第二列有一格文字橫跨
    // 原本 Age／City 兩欄的水平範圍，全表的欄邊界因此融合成一欄——這是空白
    // 欄分隔演算法在沒有表格網格線時的已知上限，不是本測試在抓 bug，
    // 是驗證這個上限被正確偵測（ambiguous == true）且文字沒有遺失。
    void mergedCellFusesColumnsAndSetsAmbiguous() {
        std::vector<TextChar> chars;
        std::int32_t index = 0;
        appendWord(chars, index, "Name", 0.0, 88.0, 100.0);
        appendWord(chars, index, "Age", 30.0, 88.0, 100.0);
        appendWord(chars, index, "City", 60.0, 88.0, 100.0);
        appendWord(chars, index, "Bob", 0.0, 68.0, 80.0);
        // 橫跨 Age（30-45）與 City（60-80）水平範圍的合併儲存格。
        appendWord(chars, index, "BigMergedCell", 30.0, 68.0, 80.0);

        PageTextLayer layer{0, chars};
        const ExtractedTable table = extractTable(layer, kWholeArea);

        QVERIFY(table.ambiguous);
        // 欄邊界因融合而少於「應有的」3 欄。
        QCOMPARE(table.columnBoundaries.size(), std::size_t{2});
        QCOMPARE(table.rows.size(), std::size_t{2});
        // 融合欄位裡兩個原本分屬不同欄的詞都要保留（以空白接起來），不能遺漏文字。
        const std::string fusedCell = table.rows[0][1];
        QVERIFY(fusedCell.find("Age") != std::string::npos);
        QVERIFY(fusedCell.find("City") != std::string::npos);
        QCOMPARE(QString::fromStdString(table.rows[1][1]), QString("BigMergedCell"));
    }

    // (d) 數字靠右對齊：同一欄裡數字的位數不同，導致每一列的「詞」左邊界都不
    // 一樣（只有右邊界對齊）。空白欄分隔看的是「這段水平範圍在所有列都沒有
    // 墨水」，不是個別詞的置中點或左邊界，因此不受靠右對齊影響——只要數字
    // 沒有真的越界侵入相鄰欄，欄依然正確分開。
    void rightAlignedNumbersDoNotConfuseColumnBoundary() {
        std::vector<TextChar> chars;
        std::int32_t index = 0;
        // 欄一：靠右對齊在 x=100，位數不同（"7" 一位、"842" 三位）。
        appendWord(chars, index, "7", 95.0, 88.0, 100.0);
        appendWord(chars, index, "842", 85.0, 68.0, 80.0);
        // 欄二：左邊界固定在 x=110，與欄一的右邊界（100）之間留有 10 點空白。
        appendWord(chars, index, "USD", 110.0, 88.0, 100.0);
        appendWord(chars, index, "USD", 110.0, 68.0, 80.0);

        PageTextLayer layer{0, chars};
        const ExtractedTable table = extractTable(layer, kWholeArea);

        QVERIFY(!table.ambiguous);
        QCOMPARE(table.columnBoundaries.size(), std::size_t{2});
        QCOMPARE(table.rows[0], (std::vector<std::string>{"7", "USD"}));
        QCOMPARE(table.rows[1], (std::vector<std::string>{"842", "USD"}));
    }

    void emptyAreaProducesEmptyTable() {
        std::vector<TextChar> chars;
        std::int32_t index = 0;
        appendWord(chars, index, "Ignored", 0.0, 0.0, 10.0);
        PageTextLayer layer{0, chars};

        const ExtractedTable table = extractTable(layer, RectF{});
        QVERIFY(table.rows.empty());
        QVERIFY(table.columnBoundaries.empty());
        QCOMPARE(QString::fromStdString(tableToTsv(table)), QString());
    }
};

QTEST_APPLESS_MAIN(TestTableExtraction)
#include "test_table_extraction.moc"
