// 尋找並取代（PRD-SRCH-003）。
//
// 這支測試補的是一塊真的沒被驗過的程式碼：find_replace 先前只有間接使用，
// 沒有任何直接測試。範圍的邊界（只動註解內文與表單欄位值、絕不動頁面內容串流）
// 是這個功能最重要的性質，因此以位元組層級驗證頁面內容串流沒有被碰過。

#include <QtTest>

#include <QTemporaryDir>

#include "engine/compare/find_replace.h"
#include "engine/objects/pdf_source_document.h"
#include "qpdf_check.h"

using namespace alioth;
using engine::compare::EditableTextKind;
using engine::compare::FindOptions;

namespace {

// 兩頁文件：
//   第 1 頁有一則 Text 註解（/Contents 含要取代的字）與一個頁面內容串流，
//          內容串流裡刻意放同一個字串——它絕對不可以被改到。
//   第 2 頁有一個表單文字欄位，/V 也含同一個字串。
QByteArray makeEditableTextPdf() {
    std::vector<QByteArray> objects;

    objects.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [9 0 R] >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Contents 5 0 R /Annots [6 0 R] >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Annots [9 0 R] >>");

    // 5：頁面內容串流。含 "SECRET"，用來驗證取代沒有碰頁面內文。
    const QByteArray stream = "BT /F1 12 Tf 20 400 Td (SECRET on the page) Tj ET\n";
    objects.push_back("<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" +
                      stream + "endstream");

    // 6：Text 註解。
    objects.push_back(
        "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /T (Alice) "
        "/Contents (SECRET appears twice: SECRET) >>");
    // 7、8：留白，讓物件編號與註記對得上。
    objects.push_back("<< /Type /Filler >>");
    objects.push_back("<< /Type /Filler >>");
    // 9：表單文字欄位（同時是 Widget 註解）。
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (secretField) /V (value has SECRET) "
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

class TestFindReplace : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        base_ = makeEditableTextPdf();
    }

    // -----------------------------------------------------------------------
    // 純字串層的比對規則
    // -----------------------------------------------------------------------

    void caseSensitivityIsRespected() {
        const std::string text = "Secret secret SECRET";
        FindOptions sensitive;
        sensitive.matchCase = true;
        QCOMPARE(engine::compare::findMatchesIn(text, 0, "secret", sensitive).size(),
                 std::size_t{1});

        FindOptions insensitive;
        QCOMPARE(engine::compare::findMatchesIn(text, 0, "secret", insensitive).size(),
                 std::size_t{3});
    }

    void wholeWordDoesNotMatchInsideAWord() {
        const std::string text = "cat catalog concat cat.";
        FindOptions options;
        options.matchWholeWord = true;
        const auto matches = engine::compare::findMatchesIn(text, 0, "cat", options);
        // "cat" 開頭、"cat." 各一次；catalog 與 concat 內部的不算。
        QCOMPARE(matches.size(), std::size_t{2});
    }

    void overlappingMatchesAdvancePastTheHit() {
        // "aa" 在 "aaaa" 裡若允許重疊會是 3 次，取代時會產生無限迴圈的風險。
        const auto matches = engine::compare::findMatchesIn("aaaa", 0, "aa", {});
        QCOMPARE(matches.size(), std::size_t{2});
    }

    void utf8OffsetsArePreserved() {
        // 位移以位元組計。中文一個字三個位元組，用字元數算會指到字的中間，
        // 取代時就會切出無效的 UTF-8。
        const std::string text = "工程圖說 SECRET 工程圖說";
        const auto matches = engine::compare::findMatchesIn(text, 0, "SECRET", {});
        QCOMPARE(matches.size(), std::size_t{1});
        QCOMPARE(text.substr(matches[0].byteOffset, matches[0].byteLength), std::string{"SECRET"});
    }

    void replaceInCountsAndSubstitutes() {
        std::int32_t count = 0;
        const std::string out =
            engine::compare::replaceIn("a SECRET and SECRET", "SECRET", "[已遮蔽]", {}, &count);
        QCOMPARE(count, 2);
        QCOMPARE(out, std::string{"a [已遮蔽] and [已遮蔽]"});
    }

    void replaceWithLongerAndShorterStrings() {
        // 取代字串比原字串長或短，都不可以讓後續命中的位移算錯。
        QCOMPARE(engine::compare::replaceIn("xAx Ax", "A", "LONGER", {}),
                 std::string{"xLONGERx LONGERx"});
        QCOMPARE(engine::compare::replaceIn("xAAAx", "AAA", "B", {}), std::string{"xBx"});
    }

    void emptyReplacementDeletesTheMatch() {
        QCOMPARE(engine::compare::replaceIn("keep SECRET keep", "SECRET ", "", {}),
                 std::string{"keep keep"});
    }

    // -----------------------------------------------------------------------
    // 文件層
    // -----------------------------------------------------------------------

    void editableTextCoversAnnotationsAndFieldsOnly() {
        engine::objects::PdfSourceDocument source;
        QVERIFY(source.open(toStd(base_)) == engine::objects::SourceStatus::Ok);

        const auto targets = engine::compare::collectEditableText(source);
        QCOMPARE(targets.size(), std::size_t{2});

        bool sawAnnotation = false;
        bool sawField = false;
        for (const engine::compare::EditableText& target : targets) {
            if (target.kind == EditableTextKind::AnnotationContents) {
                sawAnnotation = true;
                QCOMPARE(target.label, std::string{"Alice"});
            }
            if (target.kind == EditableTextKind::FormFieldValue) {
                sawField = true;
                QCOMPARE(target.label, std::string{"secretField"});
            }
        }
        QVERIFY(sawAnnotation);
        QVERIFY(sawField);
    }

    void pageContentStreamIsNeverTouched() {
        // 這是整個功能最重要的性質：頁面內文不在範圍內（PRD §2.1 排除文字編輯）。
        // 若哪天有人「順手」把取代套到內容串流，這條會失敗。
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "SECRET", "[已遮蔽]", {});
        QVERIFY2(result.ok, result.diagnostic.c_str());

        QVERIFY(result.bytes.find("SECRET on the page") != std::string::npos);
        // 註解與欄位裡的則必須都換掉：三處命中（註解兩處、欄位一處）。
        QCOMPARE(result.replacements, 3);
        QCOMPARE(result.changedObjects, 2);
    }

    void outputIsPurelyIncremental() {
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "SECRET", "X", {});
        QVERIFY(result.ok);
        QVERIFY(result.bytes.size() > static_cast<std::size_t>(base_.size()));
        QCOMPARE(QByteArray::fromStdString(
                     result.bytes.substr(0, static_cast<std::size_t>(base_.size()))),
                 base_);
    }

    void formFieldChangeRequestsAppearanceRegeneration() {
        // 改了 /V 卻沒重畫外觀，檢視器會繼續顯示舊字。我們設 /NeedAppearances，
        // 但不是所有檢視器都會照做，所以這個旗標必須傳到 UI。
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "value has", "改過的", {});
        QVERIFY(result.ok);
        QVERIFY(result.needAppearances);
        QVERIFY(result.bytes.find("NeedAppearances") != std::string::npos);
    }

    void annotationOnlyChangeDoesNotRequestAppearances() {
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "appears twice", "只有一次", {});
        QVERIFY(result.ok);
        QCOMPARE(result.replacements, 1);
        QVERIFY(!result.needAppearances);
    }

    void emptyQueryIsRejected() {
        // 空字串會在每個位置命中，靜默把檔案改成一堆重複的取代字串。
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "", "X", {});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.bytes.empty());
    }

    void noMatchProducesNoChange() {
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "不存在的字串", "X", {});
        QVERIFY(result.ok);
        QCOMPARE(result.replacements, 0);
        // 沒有命中就不該產生附加段——多寫一段空的增量只會讓檔案變大，
        // 而且會讓「檔案被改過」的判斷失準。
        QCOMPARE(result.changedObjects, 0);
    }

    void outputPassesQpdfStructureCheck() {
        const engine::compare::ReplaceResult result =
            engine::compare::replaceAll(toStd(base_), "SECRET", "已遮蔽", {});
        QVERIFY(result.ok);

        const QString path = dir_->filePath(QStringLiteral("replaced.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(result.bytes.data(), static_cast<qint64>(result.bytes.size()));
        file.close();

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QByteArray base_;
};

QTEST_MAIN(TestFindReplace)
#include "test_find_replace.moc"
