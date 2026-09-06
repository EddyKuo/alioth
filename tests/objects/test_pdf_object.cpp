// PDF 物件模型與序列化測試（ADR-002）。
//
// 這一層是純函數，因此測的是位元組本身而不是「有沒有輸出」。
// 序列化錯一個字元的後果不是崩潰，而是檔案在別的閱讀器打不開，
// 而我們自己的檢視器（PDFium）容錯度高，很可能看不出來。

#include <QtTest>

#include <clocale>
#include <string>

#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_parser.h"

using namespace alioth::engine::objects;

namespace {

QString text(const std::string& value) { return QString::fromStdString(value); }

}  // namespace

class TestPdfObject : public QObject {
    Q_OBJECT

private slots:
    void realsAreLocaleIndependent() {
        // 以逗號為小數點的地區設定會讓 printf 系列產出 "1,5"，
        // 那會讓整份串流損毀，而且只在特定使用者的機器上重現。
        const char* previous = std::setlocale(LC_NUMERIC, "de-DE");
        QCOMPARE(text(formatReal(1.5)), QStringLiteral("1.5"));
        QCOMPARE(text(formatReal(-2.25)), QStringLiteral("-2.25"));
        QCOMPARE(text(formatReal(0.0)), QStringLiteral("0"));
        QCOMPARE(text(formatReal(3.0)), QStringLiteral("3"));
        std::setlocale(LC_NUMERIC, previous != nullptr ? previous : "C");
    }

    void integersKeepIntegerSyntax() {
        // /Length 與物件編號必須是整數語法。寫成 "12.0" 會讓嚴格的解析器拒絕。
        QCOMPARE(text(serialize(PdfObject{12})), QStringLiteral("12"));
        QCOMPARE(text(serialize(PdfObject{static_cast<std::int64_t>(-7)})), QStringLiteral("-7"));
    }

    void namesAreEscaped() {
        QCOMPARE(text(serialize(makeName("Type"))), QStringLiteral("/Type"));
        QCOMPARE(text(serialize(makeName("A B"))), QStringLiteral("/A#20B"));
        QCOMPARE(text(serialize(makeName("a#b"))), QStringLiteral("/a#23b"));
        QCOMPARE(text(serialize(makeName("x/y"))), QStringLiteral("/x#2Fy"));
    }

    void literalStringsEscapeParensAndBackslash() {
        // 沒跳脫的括號會讓配對失衡，其後所有物件整段解析錯位。
        QCOMPARE(text(serialize(makeLiteralString("a(b)c"))), QStringLiteral("(a\\(b\\)c)"));
        QCOMPARE(text(serialize(makeLiteralString("back\\slash"))), QStringLiteral("(back\\\\slash)"));
        QCOMPARE(text(serialize(makeLiteralString("line\r\n"))), QStringLiteral("(line\\r\\n)"));
        QCOMPARE(text(serialize(makeLiteralString(std::string("\x01", 1)))), QStringLiteral("(\\001)"));
    }

    void asciiTextStaysLiteral() {
        QCOMPARE(text(serialize(makeTextString("Reviewer"))), QStringLiteral("(Reviewer)"));
    }

    void nonAsciiTextBecomesUtf16BeHexString() {
        // 「測試」= U+6E2C U+8A66，前面必須有 BOM，否則會被當成 PDFDocEncoding。
        QCOMPARE(text(serialize(makeTextString("測試"))), QStringLiteral("<FEFF6E2C8A66>"));
        // BMP 之外的字元要拆成代理對。U+1F600 → D83D DE00。
        QCOMPARE(text(serialize(makeTextString("\xF0\x9F\x98\x80"))), QStringLiteral("<FEFFD83DDE00>"));
    }

    void referencesAndBooleans() {
        QCOMPARE(text(serialize(makeRef(3))), QStringLiteral("3 0 R"));
        QCOMPARE(text(serialize(makeRef(12, 5))), QStringLiteral("12 5 R"));
        QCOMPARE(text(serialize(PdfObject{true})), QStringLiteral("true"));
        QCOMPARE(text(serialize(PdfObject{})), QStringLiteral("null"));
    }

    void dictionariesKeepInsertionOrder() {
        PdfDictionary dict;
        dict.set("Type", makeName("Annot"));
        dict.set("Rect", makeNumberArray({0.0, 1.5, 2.0, 3.0}));
        dict.set("F", PdfObject{4});
        QCOMPARE(text(serialize(PdfObject{dict})),
                 QStringLiteral("<</Type/Annot/Rect[0 1.5 2 3]/F 4>>"));

        // 重設既有鍵不改變順序，否則同一份輸入會產生不同的位元組。
        dict.set("Type", makeName("XObject"));
        QCOMPARE(text(serialize(PdfObject{dict})),
                 QStringLiteral("<</Type/XObject/Rect[0 1.5 2 3]/F 4>>"));
    }

    void streamsComputeTheirOwnLength() {
        PdfDictionary dict;
        dict.set("Type", makeName("XObject"));
        // 呼叫端故意給錯的 /Length：序列化器必須以實際資料長度覆寫它。
        dict.set("Length", PdfObject{999});
        const PdfObject stream{PdfStream{dict, "0 0 m\n"}};
        QCOMPARE(text(serialize(stream)),
                 QStringLiteral("<</Type/XObject/Length 6>>\nstream\n0 0 m\n\nendstream"));
    }

    void indirectObjectsWrapCorrectly() {
        QCOMPARE(text(serializeIndirect(7, 0, makeName("Foo"))),
                 QStringLiteral("7 0 obj\n/Foo\nendobj\n"));
    }

    void parserRoundTripsNestedStructures() {
        PdfDictionary inner;
        inner.set("BM", makeName("Multiply"));
        inner.set("ca", PdfObject{0.4});

        PdfDictionary dict;
        dict.set("Type", makeName("ExtGState"));
        dict.set("Nested", PdfObject{inner});
        dict.set("Ref", makeRef(9, 2));
        dict.set("Items", PdfArray{PdfObject{1}, makeLiteralString("a(b)"), PdfObject{true}});
        dict.set("Text", makeTextString("測試"));

        const std::string bytes = serialize(PdfObject{dict});
        PdfParser parser(bytes);
        PdfObject parsed;
        QVERIFY(parser.parseObject(parsed));

        const PdfDictionary* result = parsed.asDictionary();
        QVERIFY(result != nullptr);
        QCOMPARE(text(result->find("Type")->asName()), QStringLiteral("ExtGState"));
        QCOMPARE(result->find("Ref")->asRef().number, 9);
        QCOMPARE(result->find("Ref")->asRef().generation, 2);

        const PdfArray* items = result->find("Items")->asArray();
        QVERIFY(items != nullptr);
        QCOMPARE(items->size(), std::size_t{3});
        QCOMPARE(items->at(0).asInteger(), std::int64_t{1});

        // 十六進位字串解回來必須是原始位元組，不是十六進位文字本身。
        QCOMPARE(text(serialize(*result->find("Text"))), QStringLiteral("<FEFF6E2C8A66>"));
    }

    void parserReadsStreamsUsingLength() {
        const std::string bytes = "<< /Length 5 >>\nstream\nABCDE\nendstream";
        PdfParser parser(bytes);
        PdfObject parsed;
        QVERIFY(parser.parseObject(parsed));
        QVERIFY(parsed.isStream());
        QCOMPARE(text(parsed.asStream()->data), QStringLiteral("ABCDE"));
    }

    void parserSurvivesWrongLength() {
        // /Length 壞掉的檔案實務上存在。解析必須退回搜尋 endstream，
        // 而不是回傳一段長度錯誤的位元組。
        const std::string bytes = "<< /Length 99 >>\nstream\nABCDE\nendstream";
        PdfParser parser(bytes);
        PdfObject parsed;
        QVERIFY(parser.parseObject(parsed));
        QCOMPARE(text(parsed.asStream()->data), QStringLiteral("ABCDE"));
    }

    void parserRejectsGarbage() {
        const std::string bytes = "<< /Broken";
        PdfParser parser(bytes);
        PdfObject parsed;
        QVERIFY(!parser.parseObject(parsed));
    }

    void parserHandlesEscapedLiteralStrings() {
        const std::string bytes = "(a\\(b\\)c\\101\\\ntail)";
        PdfParser parser(bytes);
        PdfObject parsed;
        QVERIFY(parser.parseObject(parsed));
        QCOMPARE(text(serialize(parsed)), QStringLiteral("(a\\(b\\)cAtail)"));
    }
};

QTEST_APPLESS_MAIN(TestPdfObject)
#include "test_pdf_object.moc"
