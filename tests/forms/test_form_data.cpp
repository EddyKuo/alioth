// FDF / XFDF 序列化與解析的測試（WBS 6.3，PRD-FORM-002）。
//
// 這一組刻意不開任何 PDF：匯出匯入是純函數，語法細節（跳脫、UTF-16、
// XML 實體）值得逐字元驗證，而那種測試不該需要 PDFium。

#include <QtTest>

#include "engine/forms/form_data.h"

using namespace alioth::engine::forms;

namespace {

FormFieldInfo textField(std::string name, std::string value) {
    FormFieldInfo field;
    field.name = std::move(name);
    field.value = std::move(value);
    field.type = FormFieldType::TextField;
    return field;
}

}  // namespace

class TestFormData : public QObject {
    Q_OBJECT

private slots:
    void fdfRoundTrip() {
        std::vector<FormFieldInfo> fields{textField("fullName", "Ada Lovelace"),
                                          textField("note", "line (with) parens \\ backslash")};

        const std::string fdf = exportFormData(fields, FormDataFormat::Fdf, "C:/tmp/a.pdf");
        QVERIFY(fdf.rfind("%FDF-1.2", 0) == 0);
        QVERIFY(fdf.find("/Fields") != std::string::npos);

        const FormDataImport parsed = importFormData(fdf, FormDataFormat::Fdf);
        QVERIFY2(parsed.ok, parsed.error.c_str());
        QCOMPARE(parsed.entries.size(), std::size_t{2});
        QCOMPARE(parsed.entries[0].name, std::string("fullName"));
        QCOMPARE(parsed.entries[0].primary(), std::string("Ada Lovelace"));
        // 跳脫的括號與反斜線必須原樣還原；漏掉一個就會讓值悄悄變短。
        QCOMPARE(parsed.entries[1].primary(),
                 std::string("line (with) parens \\ backslash"));
    }

    void fdfNonAsciiUsesUtf16HexString() {
        std::vector<FormFieldInfo> fields{textField("姓名", "張小明")};
        const std::string fdf = exportFormData(fields, FormDataFormat::Fdf);
        // 非 ASCII 一律走 <FEFF...>：直接塞 UTF-8 位元組會被 Acrobat 當成
        // PDFDocEncoding，中文姓名會變成看似隨機的拉丁字母。
        QVERIFY(fdf.find("<FEFF") != std::string::npos);

        const FormDataImport parsed = importFormData(fdf, FormDataFormat::Fdf);
        QVERIFY(parsed.ok);
        QCOMPARE(parsed.entries.size(), std::size_t{1});
        QCOMPARE(parsed.entries[0].name, std::string("姓名"));
        QCOMPARE(parsed.entries[0].primary(), std::string("張小明"));
    }

    void xfdfRoundTripWithEscaping() {
        std::vector<FormFieldInfo> fields{textField("cmp", "a < b & c > d \"q\""),
                                          textField("姓名", "張小明")};
        const std::string xfdf = exportFormData(fields, FormDataFormat::Xfdf, "C:/tmp/a.pdf");
        QVERIFY(xfdf.find("<xfdf") != std::string::npos);
        QVERIFY(xfdf.find("&lt;") != std::string::npos);
        QVERIFY(xfdf.find("&amp;") != std::string::npos);

        const FormDataImport parsed = importFormData(xfdf, FormDataFormat::Xfdf);
        QVERIFY2(parsed.ok, parsed.error.c_str());
        QCOMPARE(parsed.entries.size(), std::size_t{2});
        QCOMPARE(parsed.entries[0].primary(), std::string("a < b & c > d \"q\""));
        QCOMPARE(parsed.entries[1].primary(), std::string("張小明"));
    }

    void xfdfMultiValueListBox() {
        FormFieldInfo list;
        list.name = "langs";
        list.type = FormFieldType::ListBox;
        list.options = {"zh", "en", "ja"};
        list.selectedIndices = {0, 2};

        const std::string xfdf = exportFormData({list}, FormDataFormat::Xfdf);
        const FormDataImport parsed = importFormData(xfdf, FormDataFormat::Xfdf);
        QVERIFY(parsed.ok);
        QCOMPARE(parsed.entries.size(), std::size_t{1});
        QCOMPARE(parsed.entries[0].values.size(), std::size_t{2});
        QCOMPARE(parsed.entries[0].values[0], std::string("zh"));
        QCOMPARE(parsed.entries[0].values[1], std::string("ja"));

        const std::string fdf = exportFormData({list}, FormDataFormat::Fdf);
        const FormDataImport fdfParsed = importFormData(fdf, FormDataFormat::Fdf);
        QVERIFY(fdfParsed.ok);
        QCOMPARE(fdfParsed.entries[0].values.size(), std::size_t{2});
        QCOMPARE(fdfParsed.entries[0].values[1], std::string("ja"));
    }

    void buttonsExportStateNameNotLabel() {
        FormFieldInfo check;
        check.name = "agree";
        check.type = FormFieldType::CheckBox;
        check.checked = true;
        check.exportValue = "Yes";

        const std::string xfdf = exportFormData({check}, FormDataFormat::Xfdf);
        QVERIFY(xfdf.find("<value>Yes</value>") != std::string::npos);

        check.checked = false;
        const std::string off = exportFormData({check}, FormDataFormat::Xfdf);
        QVERIFY(off.find("<value>Off</value>") != std::string::npos);
    }

    void nonExportableFieldsAreOmitted() {
        FormFieldInfo button;
        button.name = "submit";
        button.type = FormFieldType::PushButton;

        FormFieldInfo signature;
        signature.name = "sig";
        signature.type = FormFieldType::Signature;
        signature.value = "<binary>";

        FormFieldInfo noExport = textField("secret", "hidden");
        noExport.flags.noExport = true;

        QVERIFY(!isExportable(button));
        QVERIFY(!isExportable(signature));
        QVERIFY(!isExportable(noExport));

        const std::string xfdf =
            exportFormData({button, signature, noExport, textField("ok", "v")},
                           FormDataFormat::Xfdf);
        QVERIFY(xfdf.find("submit") == std::string::npos);
        QVERIFY(xfdf.find("\"sig\"") == std::string::npos);
        QVERIFY(xfdf.find("secret") == std::string::npos);
        QVERIFY(xfdf.find("\"ok\"") != std::string::npos);
    }

    void radioWidgetsDeduplicateByName() {
        FormFieldInfo a;
        a.name = "gender";
        a.type = FormFieldType::RadioButton;
        a.checked = true;
        a.exportValue = "F";
        FormFieldInfo b = a;
        b.checked = false;
        b.exportValue = "M";

        const std::string xfdf = exportFormData({a, b}, FormDataFormat::Xfdf);
        // 一個單選群組只能出現一次，否則收檔端會拿到互相矛盾的兩筆值。
        std::size_t count = 0;
        for (std::size_t pos = xfdf.find("<field name="); pos != std::string::npos;
             pos = xfdf.find("<field name=", pos + 1)) {
            ++count;
        }
        QCOMPARE(count, std::size_t{1});
    }

    void detectFormatRefusesToGuess() {
        QVERIFY(detectFormat("%FDF-1.2\n").has_value());
        QCOMPARE(*detectFormat("%FDF-1.2\n"), FormDataFormat::Fdf);
        QCOMPARE(*detectFormat("  <?xml version=\"1.0\"?><xfdf/>"), FormDataFormat::Xfdf);
        QCOMPARE(*detectFormat("<xfdf xmlns=\"\"/>"), FormDataFormat::Xfdf);
        // 認不出來就說認不出來。猜一個會產生一堆看似成功的空欄位。
        QVERIFY(!detectFormat("name,value\nA,1\n").has_value());
        QVERIFY(!detectFormat("").has_value());
    }

    void malformedInputReportsError() {
        const FormDataImport fdf = importFormData("not a fdf at all", FormDataFormat::Fdf);
        QVERIFY(!fdf.ok);
        QVERIFY(!fdf.error.empty());

        const FormDataImport xfdf = importFormData("<html><body/></html>", FormDataFormat::Xfdf);
        QVERIFY(!xfdf.ok);
        QVERIFY(!xfdf.error.empty());
    }

    void xfdfNumericEntitiesAreDecoded() {
        const std::string xfdf =
            "<?xml version=\"1.0\"?><xfdf><fields>"
            "<field name=\"t\"><value>&#24373;&#x5C0F;</value></field>"
            "</fields></xfdf>";
        const FormDataImport parsed = importFormData(xfdf, FormDataFormat::Xfdf);
        QVERIFY(parsed.ok);
        QCOMPARE(parsed.entries[0].primary(), std::string("張小"));
    }
};

QTEST_APPLESS_MAIN(TestFormData)
#include "test_form_data.moc"
