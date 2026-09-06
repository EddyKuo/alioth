// CSV 讀寫（domain/csv.h）。PRD-FORM-023「CSV 灌入表單」的解析依賴這裡，
// 因此補上回讀測試：引號、逗號、換行、BOM 都要能寫出去再讀回來等於原值。

#include <QtTest>

#include "domain/csv.h"

using namespace alioth::domain;

class TestCsv : public QObject {
    Q_OBJECT

private slots:
    void roundTripPlainFields() {
        const std::vector<std::vector<std::string>> rows = {{"a", "b", "c"}, {"1", "2", "3"}};
        const std::string doc = csvDocument(rows);
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument(doc);
        QCOMPARE(parsed.size(), rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            QCOMPARE(QString::fromStdString(csvRow(parsed[i])), QString::fromStdString(csvRow(rows[i])));
        }
    }

    void roundTripFieldsWithCommaQuoteAndNewline() {
        const std::vector<std::vector<std::string>> rows = {
            {"plain", "has,comma", "has\"quote", "has\nnewline", "has\r\ncrlf"}};
        const std::string doc = csvDocument(rows);
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument(doc);
        QCOMPARE(parsed.size(), std::size_t{1});
        QCOMPARE(parsed.front().size(), rows.front().size());
        for (std::size_t i = 0; i < rows.front().size(); ++i) {
            QCOMPARE(QString::fromStdString(parsed.front()[i]), QString::fromStdString(rows.front()[i]));
        }
    }

    void stripsUtf8Bom() {
        const std::string withBom =
            std::string("\xEF\xBB\xBF") + "name,age\r\nAlice,30\r\n";
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument(withBom);
        QCOMPARE(parsed.size(), std::size_t{2});
        // 表頭第一欄不該殘留 BOM 位元組，否則跟表單欄位名永遠比對不上。
        QCOMPARE(QString::fromStdString(parsed[0][0]), QStringLiteral("name"));
    }

    void stripUtf8BomIsIdempotentWithoutBom() {
        const std::string noBom = "a,b\r\n";
        QCOMPARE(QString::fromStdString(stripUtf8Bom(noBom)), QString::fromStdString(noBom));
    }

    void emptyDocumentParsesToNoRows() {
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument("");
        QVERIFY(parsed.empty());
    }

    void trailingRowWithoutFinalNewlineIsKept() {
        const std::vector<std::vector<std::string>> parsed = parseCsvDocument("a,b\r\nc,d");
        QCOMPARE(parsed.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(parsed[1][0]), QStringLiteral("c"));
        QCOMPARE(QString::fromStdString(parsed[1][1]), QStringLiteral("d"));
    }
};

QTEST_MAIN(TestCsv)
#include "test_csv.moc"
