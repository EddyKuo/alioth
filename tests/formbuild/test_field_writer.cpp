// 表單欄位建立（PRD-FORM-010~020）。
//
// PDFium 沒有建立表單欄位的公開 API，所以這條路是自己寫 PDF 字典。
// 自己寫的東西必須由**別人**讀回來才算數：這裡用 alioth_forms（走 PDFium 的
// 表單 API）驗證，而不是用自己的解析器——後者只會證明我們前後一致地寫錯。

#include <QtTest>

#include <QTemporaryDir>

#include <atomic>
#include <string>

#include "engine/forms/form_document.h"
#include "engine/formbuild/form_field_writer.h"
#include "engine/objects/incremental_appender.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::formbuild;

namespace {

// 一份沒有任何表單欄位的兩頁 PDF。欄位全部由測試自己加上去。
std::string blankDocument() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> >>");

    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

FieldDefinition textField(const std::string& name, std::int32_t page = 0) {
    FieldDefinition definition;
    definition.type = BuildFieldType::Text;
    definition.name = name;
    definition.pageIndex = page;
    definition.rectPt = domain::RectF{50.0, 700.0, 300.0, 724.0};
    return definition;
}

}  // namespace

class TestFieldWriter : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        source_ = blankDocument();
    }

    void allNineFieldTypesAreWritten() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY2(writer.ready(), writer.diagnostic().c_str());

        const auto add = [&writer](FieldDefinition definition) {
            const FieldWriteResult result = writer.addField(definition);
            QVERIFY2(result.ok, result.diagnostic.c_str());
            QVERIFY2(result.fieldObject > 0, "欄位字典沒有配到物件編號");
            // 沒有 /AP 的欄位在 Acrobat 以外的檢視器可能整個不顯示。
            QVERIFY2(!result.appearanceObjects.empty(), "欄位沒有外觀串流");
        };

        add(textField("text.plain"));

        FieldDefinition check;
        check.type = BuildFieldType::CheckBox;
        check.name = "check.agree";
        check.rectPt = domain::RectF{50.0, 660.0, 66.0, 676.0};
        check.checked = true;
        add(check);

        FieldDefinition radio;
        radio.type = BuildFieldType::RadioGroup;
        radio.name = "radio.choice";
        radio.radios.push_back(RadioOption{"optionA", domain::RectF{50.0, 620.0, 66.0, 636.0}});
        radio.radios.push_back(RadioOption{"optionB", domain::RectF{80.0, 620.0, 96.0, 636.0}});
        add(radio);

        FieldDefinition combo;
        combo.type = BuildFieldType::ComboBox;
        combo.name = "combo.country";
        combo.rectPt = domain::RectF{50.0, 580.0, 250.0, 604.0};
        combo.options = {"Taiwan", "Japan"};
        add(combo);

        FieldDefinition list;
        list.type = BuildFieldType::ListBox;
        list.name = "list.items";
        list.rectPt = domain::RectF{50.0, 500.0, 250.0, 570.0};
        list.options = {"one", "two", "three"};
        add(list);

        FieldDefinition button;
        button.type = BuildFieldType::PushButton;
        button.name = "button.submit";
        button.rectPt = domain::RectF{50.0, 460.0, 150.0, 484.0};
        add(button);

        FieldDefinition date;
        date.type = BuildFieldType::Date;
        date.name = "date.signed";
        date.rectPt = domain::RectF{50.0, 420.0, 200.0, 444.0};
        add(date);

        FieldDefinition image;
        image.type = BuildFieldType::Image;
        image.name = "image.photo";
        image.rectPt = domain::RectF{50.0, 320.0, 150.0, 400.0};
        add(image);

        FieldDefinition signature;
        signature.type = BuildFieldType::Signature;
        signature.name = "signature.approver";
        signature.rectPt = domain::RectF{300.0, 320.0, 500.0, 380.0};
        add(signature);

        // finish() 回傳的是診斷字串，空字串代表成功——檔案位元組要跟 appender 拿。
        // 這個介面很容易誤讀成「回傳檔案內容」，用錯時的症狀是拿到一個空字串
        // 或一段錯誤訊息去當 PDF 寫檔。
        QVERIFY2(writer.finish().empty(), "finish() 回報了錯誤");
        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        const std::string& bytes = built.bytes;

        // 純附加：原檔前綴逐位元組不變，既有簽章不會失效。
        QVERIFY(bytes.size() > source_.size());
        QCOMPARE(bytes.compare(0, source_.size(), source_), 0);

        // 由 PDFium 的表單 API 讀回來——這才是「別人看得到」的證明。
        const QString path = dir_->filePath(QStringLiteral("fields.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes.data(), static_cast<qint64>(bytes.size()));
        file.close();

        engine::forms::FormDocument document;
        std::atomic<int> error{-1};
        document.open(path.toStdString(), "",
                      [&error](domain::DocumentError e) { error = static_cast<int>(e); });
        document.waitForIdle();
        QCOMPARE(error.load(), static_cast<int>(domain::DocumentError::None));

        std::vector<std::string> names;
        document.allFields([&names](const std::vector<engine::forms::FormFieldInfo>& fields) {
            for (const engine::forms::FormFieldInfo& f : fields) names.push_back(f.name);
        });
        document.waitForIdle();

        QVERIFY2(names.size() >= 9,
                 qPrintable(QStringLiteral("PDFium 只讀到 %1 個欄位，預期至少 9 個")
                                .arg(names.size())));
    }

    void duplicateNamesAreRejected() {
        // 同名欄位在 PDF 裡代表「同一個欄位的多個外觀」，值會連動。
        // 使用者以為建了兩個獨立欄位，實際填一個另一個跟著變。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());
        QVERIFY(writer.addField(textField("same.name")).ok);

        const FieldWriteResult second = writer.addField(textField("same.name"));
        QVERIFY2(!second.ok, "重複的欄位名被接受了");
        QVERIFY(!second.diagnostic.empty());
    }

    void fieldsOnDifferentPagesLandOnTheRightPage() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());
        QVERIFY(writer.addField(textField("on.first", 0)).ok);
        QVERIFY(writer.addField(textField("on.second", 1)).ok);

        QVERIFY(writer.finish().empty());
        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        const std::string& bytes = built.bytes;

        const QString path = dir_->filePath(QStringLiteral("pages.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes.data(), static_cast<qint64>(bytes.size()));
        file.close();

        engine::forms::FormDocument document;
        std::atomic<int> error{-1};
        document.open(path.toStdString(), "",
                      [&error](domain::DocumentError e) { error = static_cast<int>(e); });
        document.waitForIdle();
        QCOMPARE(error.load(), static_cast<int>(domain::DocumentError::None));

        std::vector<std::pair<std::string, int>> placed;
        document.allFields([&placed](const std::vector<engine::forms::FormFieldInfo>& fields) {
            for (const engine::forms::FormFieldInfo& f : fields) {
                placed.emplace_back(f.name, f.pageIndex);
            }
        });
        document.waitForIdle();

        QCOMPARE(placed.size(), std::size_t{2});
        for (const auto& [name, page] : placed) {
            if (name == "on.first") QCOMPARE(page, 0);
            if (name == "on.second") QCOMPARE(page, 1);
        }
    }

    void invalidPageIndexIsRejected() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);
        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());

        const FieldWriteResult result = writer.addField(textField("out.of.range", 99));
        QVERIFY(!result.ok);
    }

    void tabOrderIsWrittenWhenAbsent() {
        // 見 page_object_editor.h：頁面原本沒有 /Tabs 時，加欄位後應該補上
        // /Tabs /W（依 /Annots 裡 Widget 出現順序決定跳位順序）。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());
        QVERIFY(writer.addField(textField("tabs.first")).ok);
        QVERIFY(writer.finish().empty());

        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        // 新增的頁面物件版本會出現在增量段（原檔前綴之後），檢查那一段裡
        // 有沒有 /Tabs/W 即可，不需要真的解析物件結構。序列化器對「值是名稱」
        // 這種自帶分隔符的情況不會另外插空白，所以鍵與值之間沒有空格。
        const std::string appended = built.bytes.substr(source_.size());
        QVERIFY2(appended.find("/Tabs/W") != std::string::npos,
                 "增量段裡沒有找到 /Tabs/W");
    }

    void existingTabOrderIsNotOverwritten() {
        // 頁面已經指定 /Tabs（例如原作者選了結構順序）時，不應該被我們的
        // /W 覆蓋——那是既有的、可能是刻意的選擇。
        std::vector<std::string> objects;
        objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
        objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> /Tabs /S >>");

        std::string pdf = "%PDF-1.7\n";
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            offsets.push_back(pdf.size());
            pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const std::size_t xref = pdf.size();
        pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
        for (const std::size_t offset : offsets) {
            std::string digits = std::to_string(offset);
            pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
        }
        pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
               " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";

        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(pdf) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());
        QVERIFY(writer.addField(textField("tabs.preset")).ok);
        QVERIFY(writer.finish().empty());

        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        const std::string appended = built.bytes.substr(pdf.size());
        QVERIFY2(appended.find("/Tabs/W") == std::string::npos,
                 "既有的 /Tabs 被覆寫了");
    }

    void textFieldWithValuePassesQpdfCheck() {
        // 帶預設值的文字欄位會走外觀串流的文字繪製路徑。那條路徑曾經漏掉
        // 常值字串的外層括號——PDFium 容忍並照樣顯示，只有 qpdf 抓得到。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());

        FieldDefinition definition = textField("filled.field");
        // 注意：C++ 字面值裡 "\b" 是退格控制字元（0x08），不是字母 b——
        // 這裡曾經誤寫成 "\backslash"，在舊版「靜默丟棄非 ASCII」的
        // foldToWinAnsi 底下不會被發現（退格字元被悄悄丟掉），改成明確失敗
        // 之後這個測試資料裡的錯字才浮現。要測的是字面上的反斜線字元，
        // 因此這裡改用 "\\" 跳脫。
        definition.value = "Sample (with parens) and \\backslash";
        QVERIFY(writer.addField(definition).ok);
        QVERIFY(writer.finish().empty());

        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = dir_->filePath(QStringLiteral("filled.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(built.bytes.data(), static_cast<qint64>(built.bytes.size()));
        file.close();

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

    void outputPassesQpdfStructureCheck() {
        // 自己寫的字典最容易出的錯是結構層的，而我們自己的檢視器（PDFium）
        // 容錯度高，看不出來。qpdf 是外部裁判。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(source_) == engine::objects::SourceStatus::Ok);

        FormFieldWriter writer(appender);
        QVERIFY(writer.ready());
        QVERIFY(writer.addField(textField("checked.field")).ok);
        QVERIFY(writer.finish().empty());
        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);
        const std::string& bytes = built.bytes;

        const QString path = dir_->filePath(QStringLiteral("qpdf.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes.data(), static_cast<qint64>(bytes.size()));
        file.close();

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    std::string source_;
};

QTEST_MAIN(TestFieldWriter)
#include "test_field_writer.moc"
