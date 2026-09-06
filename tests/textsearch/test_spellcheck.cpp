// 拼字檢查測試（PRD-SRCH-004）。見 engine/compare/spellcheck.h 檔頭：
// 字典來源本身是 BLOCKED（沒有可上線的字典），這裡測的是分詞、字典查詢、
// 建議演算法、自訂詞／忽略清單、以及「只查註解與表單、不查頁面內文」這條
// 範圍邊界——這些都不受字典來源阻塞，也是唯一能在字典換掉之後繼續有效的部分。

#include <QtTest>

#include <algorithm>
#include <string>
#include <vector>

#include "engine/compare/spellcheck.h"
#include "engine/objects/pdf_source_document.h"

using namespace alioth::engine::compare;
using alioth::engine::objects::PdfSourceDocument;
using alioth::engine::objects::SourceStatus;

namespace {

// 一頁文件：
//   頁面內容串流含 "SECRETT"（刻意拼錯）——驗證這個錯字絕對不會被回報，
//   因為頁面內文本來就不在拼字檢查的範圍內。
//   一則 Text 註解，/Contents 裡有一個拼錯的英文字 "documnet"。
//   一個表單文字欄位，/V 裡有一個拼錯的英文字 "Helo"。
QByteArray makeSpellCheckPdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [9 0 R] >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Contents 5 0 R /Annots [6 0 R] >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Annots [9 0 R] >>");

    const QByteArray stream = "BT /F1 12 Tf 20 400 Td (SECRETT on the page) Tj ET\n";
    objects.push_back("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" +
                      stream + "endstream");

    objects.push_back(
        "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /T (Alice) "
        "/Contents (This is a documnet with a typo.) >>");
    objects.push_back("<< /Type /Filler >>");
    objects.push_back("<< /Type /Filler >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (greeting) /V (Helo world) "
        "/Rect [10 100 200 130] /F 4 >>");

    QByteArray pdf = "%PDF-1.7\n";
    std::vector<int> offsets;
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[static_cast<std::size_t>(i)] +
               "\nendobj\n";
    }
    const int xref = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           "\n0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace

class TestSpellCheck : public QObject {
    Q_OBJECT

private slots:
    void tokenizerSkipsNonAsciiWordsAndKeepsInternalPunctuation() {
        const auto tokens = tokenizeForSpelling("don't co-worker 12345 你好 hello, world!");
        std::vector<std::string> words;
        for (const auto& t : tokens) words.push_back(t.text);
        QCOMPARE(words, (std::vector<std::string>{"don't", "co-worker", "hello", "world"}));
    }

    void tokenizerReportsCorrectByteOffsets() {
        const std::string text = "  hello world";
        const auto tokens = tokenizeForSpelling(text);
        QCOMPARE(tokens.size(), std::size_t{2});
        QCOMPARE(tokens[0].byteOffset, std::size_t{2});
        QCOMPARE(tokens[0].byteLength, std::size_t{5});
        QCOMPARE(tokens[1].byteOffset, std::size_t{8});
    }

    void dictionaryKnowsSampleWordsCaseInsensitively() {
        const InMemoryDictionary dict = makeSampleDictionary();
        QVERIFY(dict.contains("hello"));
        QVERIFY(!dict.contains("documnet"));
    }

    void isKnownRespectsCustomWordsAndIgnoreList() {
        const InMemoryDictionary dict = makeSampleDictionary();
        SpellCheckSession session(dict);
        QVERIFY(!session.isKnown("aliothxyz"));

        session.addCustomWord("AliothXYZ");
        QVERIFY(session.isKnown("aliothxyz"));
        QVERIFY(session.isKnown("ALIOTHXYZ"));  // 大小寫不敏感

        SpellCheckSession another(dict);
        QVERIFY(!another.isKnown("weirdword"));
        another.ignoreWord("weirdword");
        QVERIFY(another.isKnown("weirdword"));
    }

    void suggestFindsEditDistanceOneCandidates() {
        const InMemoryDictionary dict = makeSampleDictionary();
        SpellCheckSession session(dict);

        // documnet → document 只差一次相鄰換位（e/n 對調）。
        const auto documentSuggestions = session.suggest("documnet");
        QVERIFY(std::find(documentSuggestions.begin(), documentSuggestions.end(), "document") !=
                documentSuggestions.end());

        // Helo → hello 只差插入一個 'l'。
        const auto helloSuggestions = session.suggest("Helo");
        QVERIFY(std::find(helloSuggestions.begin(), helloSuggestions.end(), "hello") !=
                helloSuggestions.end());
    }

    void customWordsAreNotUsedAsSuggestionSource() {
        // 自訂詞只代表「別再問我」，不是字典認得的正確拼法，因此不該出現在
        // 別的錯字的建議清單裡（否則會把打錯的專有名詞越修越錯）。
        const InMemoryDictionary dict = makeSampleDictionary();
        SpellCheckSession session(dict);
        session.addCustomWord("zzzzz");
        const auto suggestions = session.suggest("zzzz");  // 編輯距離 1 就能到 "zzzzz"
        QVERIFY(std::find(suggestions.begin(), suggestions.end(), "zzzzz") == suggestions.end());
    }

    void checkTextFlagsUnknownWordsWithSuggestions() {
        const InMemoryDictionary dict = makeSampleDictionary();
        SpellCheckSession session(dict);
        const auto issues = session.checkText("This is a documnet with a typo.");

        bool found = false;
        for (const SpellIssue& issue : issues) {
            if (issue.word == "documnet") {
                found = true;
                QVERIFY(std::find(issue.suggestions.begin(), issue.suggestions.end(),
                                  "document") != issue.suggestions.end());
            }
        }
        QVERIFY(found);
    }

    // 範圍邊界：只查註解內文與表單欄位值，絕不查頁面內文。
    void checkDocumentOnlyCoversAnnotationsAndFormFieldsNotPageContent() {
        const std::string pdf = toStd(makeSpellCheckPdf());
        PdfSourceDocument source;
        std::string diagnostic;
        QCOMPARE(static_cast<int>(source.open(pdf, &diagnostic)),
                 static_cast<int>(SourceStatus::Ok));

        const InMemoryDictionary dict = makeSampleDictionary();
        SpellCheckSession session(dict);
        const std::vector<SpellIssue> issues = session.checkDocument(source);

        bool foundDocumnet = false;
        bool foundHelo = false;
        bool foundSecrett = false;
        for (const SpellIssue& issue : issues) {
            if (issue.word == "documnet") foundDocumnet = true;
            if (issue.word == "Helo") foundHelo = true;
            if (issue.word == "SECRETT") foundSecrett = true;
        }
        QVERIFY(foundDocumnet);
        QVERIFY(foundHelo);
        QVERIFY(!foundSecrett);  // 頁面內文的錯字絕不回報
    }
};

QTEST_APPLESS_MAIN(TestSpellCheck)
#include "test_spellcheck.moc"
