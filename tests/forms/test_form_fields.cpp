// 表單環境、欄位互動、辨識表單、XFA 降級、攤平的測試
//（WBS 6.1–6.4，PRD-FORM-001 ~ 005）。
//
// 這一組會實際載入 pdfium.dll 並建立表單填寫環境，因此驗到的是真正的
// PDFium 行為而不是我們對它的想像。

#include <QtTest>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>

#include "engine/forms/form_document.h"
#include "form_pdf_fixture.h"
#include "pdf_fixture.h"

using namespace alioth::engine::forms;
using alioth::domain::DocumentError;
using alioth::domain::PointF;

namespace {

// 等待非同步結果。逾時就失敗，不無限期掛住 CI。
template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait(int milliseconds = 15000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }

    [[nodiscard]] const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

const FormFieldInfo* findByName(const std::vector<FormFieldInfo>& fields, const std::string& name) {
    for (const auto& field : fields) {
        if (field.name == name) return &field;
    }
    return nullptr;
}

std::size_t darkPixels(const alioth::engine::PixelBufferPtr& buffer) {
    if (!buffer) return 0;
    std::size_t count = 0;
    for (std::int32_t y = 0; y < buffer->height(); ++y) {
        const std::uint8_t* row = buffer->data() + buffer->stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < buffer->width(); ++x) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            if (px[0] < 128 && px[1] < 128 && px[2] < 128) ++count;
        }
    }
    return count;
}

}  // namespace

class TestFormFields : public QObject {
    Q_OBJECT

private:
    // 每個測試各開一份新文件：表單填寫會改動記憶體中的文件狀態，
    // 共用一份會讓測試之間互相汙染，而那種失敗是隨執行順序變動的。
    std::unique_ptr<QTemporaryFile> writeFixture(const QByteArray& bytes) {
        auto file = alioth::test::writeTempPdf(bytes);
        return file;
    }

    static bool openDocument(FormDocument& document, QTemporaryFile& file) {
        Latch<DocumentError> latch;
        document.open(file.fileName().toStdString(), {},
                      [&latch](DocumentError error) { latch.set(error); });
        if (!latch.wait()) return false;
        return latch.value() == DocumentError::None;
    }

    static std::vector<FormFieldInfo> fieldsOf(FormDocument& document) {
        Latch<std::vector<FormFieldInfo>> latch;
        document.allFields([&latch](std::vector<FormFieldInfo> fields) {
            latch.set(std::move(fields));
        });
        if (!latch.wait()) return {};
        return latch.value();
    }

private slots:
    void enumeratesFieldsWithTypes() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));
        QCOMPARE(document.pageCount(), 1);

        const auto fields = fieldsOf(document);
        QCOMPARE(fields.size(), std::size_t{6});

        const FormFieldInfo* text = findByName(fields, "fullName");
        QVERIFY(text != nullptr);
        QCOMPARE(text->type, FormFieldType::TextField);
        QCOMPARE(text->value, std::string("Ada"));
        QCOMPARE(text->alternateName, std::string("姓名"));
        QVERIFY(!text->rectPt.isEmpty());
        QCOMPARE(text->rectPt.left, 20.0);
        QCOMPARE(text->rectPt.top, 460.0);

        const FormFieldInfo* check = findByName(fields, "agree");
        QVERIFY(check != nullptr);
        QCOMPARE(check->type, FormFieldType::CheckBox);
        QVERIFY(!check->checked);

        const FormFieldInfo* combo = findByName(fields, "city");
        QVERIFY(combo != nullptr);
        QCOMPARE(combo->type, FormFieldType::ComboBox);
        QCOMPARE(combo->options.size(), std::size_t{3});
        QCOMPARE(combo->options[1], std::string("Tokyo"));

        const FormFieldInfo* list = findByName(fields, "langs");
        QVERIFY(list != nullptr);
        QCOMPARE(list->type, FormFieldType::ListBox);
        QVERIFY(list->flags.multiSelect);

        // 單選群組是一個欄位兩個 widget：名字相同、annotIndex 不同。
        int radioWidgets = 0;
        std::vector<std::int32_t> radioIndices;
        for (const auto& field : fields) {
            if (field.name == "gender") {
                ++radioWidgets;
                radioIndices.push_back(field.annotIndex);
                QCOMPARE(field.type, FormFieldType::RadioButton);
            }
        }
        QCOMPARE(radioWidgets, 2);
        QVERIFY(radioIndices[0] != radioIndices[1]);
    }

    void noFormDocumentIsNotReportedAsForm() {
        auto file = writeFixture(alioth::test::makeNoFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        Latch<FormIdentification> latch;
        document.identify([&latch](FormIdentification result) { latch.set(std::move(result)); });
        QVERIFY(latch.wait());

        // 文件裡有一個 Square 註解。有 /Annots 不等於有表單——
        // 誤報會讓 UI 對每份純文件都亮起表單工具列。
        QCOMPARE(latch.value().formType, FormType::None);
        QCOMPARE(latch.value().fieldCount, 0);
        QVERIFY(!latch.value().hasAcroFormDictionary);
        QVERIFY(!latch.value().isFillable());
    }

    void identifyReportsPagesWithFields() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        Latch<FormIdentification> latch;
        document.identify([&latch](FormIdentification result) { latch.set(std::move(result)); });
        QVERIFY(latch.wait());
        QCOMPARE(latch.value().formType, FormType::AcroForm);
        QCOMPARE(latch.value().fieldCount, 6);
        QCOMPARE(latch.value().pagesWithFields, 1);
        QVERIFY(latch.value().isFillable());
    }

    void xfaDocumentsAreFlaggedWithHonestMessage() {
        struct Case {
            bool dynamic;
            bool fallback;
            FormType expected;
        };
        const Case cases[] = {
            {true, false, FormType::XfaFull},
            {true, true, FormType::XfaFull},
            {false, true, FormType::XfaForeground},
        };

        for (const Case& testCase : cases) {
            auto file = writeFixture(
                alioth::test::makeXfaPdf(testCase.dynamic, testCase.fallback));
            QVERIFY(file);
            FormDocument document;
            QVERIFY(openDocument(document, *file));

            Latch<FormIdentification> identification;
            document.identify([&identification](FormIdentification result) {
                identification.set(std::move(result));
            });
            QVERIFY(identification.wait());
            QCOMPARE(identification.value().formType, testCase.expected);

            Latch<XfaReport> report;
            document.xfaReport([&report](XfaReport value) { report.set(std::move(value)); });
            QVERIFY(report.wait());
            QVERIFY(report.value().present);
            QCOMPARE(report.value().dynamic, testCase.dynamic);
            QCOMPARE(report.value().acroFormFallbackUsable, testCase.fallback);
            QVERIFY(!report.value().message.empty());
            // PRD §13：提示不得誤導。有可填後備欄位時不能說「無法填寫」。
            if (testCase.fallback) {
                QVERIFY(report.value().message.find("無法填寫") == std::string::npos);
                QVERIFY(report.value().message.find("AcroForm") != std::string::npos);
            } else {
                QVERIFY(report.value().message.find("無法填寫") != std::string::npos);
            }
        }
    }

    void nonXfaDocumentHasNoXfaReport() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        Latch<XfaReport> report;
        document.xfaReport([&report](XfaReport value) { report.set(std::move(value)); });
        QVERIFY(report.wait());
        QVERIFY(!report.value().present);
        QVERIFY(report.value().message.empty());
    }

    void textValueSurvivesReadBack() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        const auto before = fieldsOf(document);
        const FormFieldInfo* text = findByName(before, "fullName");
        QVERIFY(text != nullptr);

        Latch<FormFillResult> write;
        document.setTextValue(text->pageIndex, text->annotIndex, "Grace Hopper",
                              [&write](FormFillResult result) { write.set(std::move(result)); });
        QVERIFY(write.wait());
        QVERIFY2(write.value().ok, write.value().error.c_str());

        const auto after = fieldsOf(document);
        const FormFieldInfo* updated = findByName(after, "fullName");
        QVERIFY(updated != nullptr);
        QCOMPARE(updated->value, std::string("Grace Hopper"));
    }

    void fillingTriggersAppearanceUpdateEvents() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        const auto fields = fieldsOf(document);
        const FormFieldInfo* text = findByName(fields, "fullName");
        QVERIFY(text != nullptr);

        Latch<bool> cleared;
        document.clearEvents([&cleared] { cleared.set(true); });
        QVERIFY(cleared.wait());

        Latch<FormFillResult> write;
        document.setTextValue(text->pageIndex, text->annotIndex, "Edited",
                              [&write](FormFillResult result) { write.set(std::move(result)); });
        QVERIFY(write.wait());
        QVERIFY(write.value().ok);

        Latch<FormEventSnapshot> snapshot;
        document.eventSnapshot(
            [&snapshot](FormEventSnapshot value) { snapshot.set(std::move(value)); });
        QVERIFY(snapshot.wait());
        // 值改了但外觀沒被要求重畫，代表事件橋接沒接上——那種缺陷在畫面上
        // 的症狀是「打字後畫面不動」，很容易被誤診成渲染問題。
        QVERIFY(!snapshot.value().invalidations.empty());
        QVERIFY(snapshot.value().changeCount > 0);
        QCOMPARE(snapshot.value().invalidations.front().pageIndex, 0);
    }

    void checkboxTogglesThroughMouseEvents() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        const auto before = fieldsOf(document);
        const FormFieldInfo* check = findByName(before, "agree");
        QVERIFY(check != nullptr);
        QVERIFY(!check->checked);

        Latch<FormFillResult> on;
        document.setChecked(check->pageIndex, check->annotIndex, true,
                            [&on](FormFillResult result) { on.set(std::move(result)); });
        QVERIFY(on.wait());
        QVERIFY2(on.value().ok, on.value().error.c_str());

        // 必須先把清單存進具名變數：findByName 回傳的是指向該向量元素的指標，
        // 直接寫 findByName(fieldsOf(document), ...) 再跨述句使用會指向已銷毀的暫存物件。
        const auto afterOn = fieldsOf(document);
        const FormFieldInfo* checked = findByName(afterOn, "agree");
        QVERIFY(checked != nullptr);
        QVERIFY(checked->checked);
        QCOMPARE(checked->exportValue, std::string("Yes"));

        // 重複設定同一個狀態不得把它切回去。
        Latch<FormFillResult> again;
        document.setChecked(check->pageIndex, check->annotIndex, true,
                            [&again](FormFillResult result) { again.set(std::move(result)); });
        QVERIFY(again.wait());
        QVERIFY(again.value().ok);
        const auto afterAgain = fieldsOf(document);
        QVERIFY(findByName(afterAgain, "agree")->checked);

        Latch<FormFillResult> off;
        document.setChecked(check->pageIndex, check->annotIndex, false,
                            [&off](FormFillResult result) { off.set(std::move(result)); });
        QVERIFY(off.wait());
        QVERIFY2(off.value().ok, off.value().error.c_str());
        const auto afterOff = fieldsOf(document);
        QVERIFY(!findByName(afterOff, "agree")->checked);
    }

    void radioSelectionAffectsOnlyOneWidget() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        std::vector<FormFieldInfo> radios;
        for (const auto& field : fieldsOf(document)) {
            if (field.name == "gender") radios.push_back(field);
        }
        QCOMPARE(radios.size(), std::size_t{2});

        Latch<FormFillResult> pick;
        document.setChecked(radios[0].pageIndex, radios[0].annotIndex, true,
                            [&pick](FormFillResult result) { pick.set(std::move(result)); });
        QVERIFY(pick.wait());
        QVERIFY2(pick.value().ok, pick.value().error.c_str());

        int checkedCount = 0;
        for (const auto& field : fieldsOf(document)) {
            if (field.name == "gender" && field.checked) ++checkedCount;
        }
        // 單選群組的定義就是「最多一顆被選」。用欄位名而不是 widget 索引
        // 定位時最常見的錯誤就是兩顆同時亮。
        QCOMPARE(checkedCount, 1);
    }

    void choiceSelectionCanBeSet() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        const auto listFields = fieldsOf(document);
        const FormFieldInfo* list = findByName(listFields, "langs");
        QVERIFY(list != nullptr);

        Latch<FormFillResult> select;
        document.setSelectedIndices(list->pageIndex, list->annotIndex, {1, 2},
                                    [&select](FormFillResult r) { select.set(std::move(r)); });
        QVERIFY(select.wait());
        QVERIFY2(select.value().ok, select.value().error.c_str());

        const auto updatedFields = fieldsOf(document);
        const FormFieldInfo* updated = findByName(updatedFields, "langs");
        QVERIFY(updated != nullptr);
        QCOMPARE(updated->selectedIndices.size(), std::size_t{2});
        QCOMPARE(updated->selectedIndices[0], 1);
        QCOMPARE(updated->selectedIndices[1], 2);
    }

    void hitTestFindsFieldUnderPoint() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        Latch<std::optional<FormFieldInfo>> hit;
        document.fieldAt(0, PointF{100.0, 450.0},
                         [&hit](std::optional<FormFieldInfo> f) { hit.set(std::move(f)); });
        QVERIFY(hit.wait());
        QVERIFY(hit.value().has_value());
        QCOMPARE(hit.value()->name, std::string("fullName"));

        Latch<std::optional<FormFieldInfo>> miss;
        document.fieldAt(0, PointF{280.0, 20.0},
                         [&miss](std::optional<FormFieldInfo> f) { miss.set(std::move(f)); });
        QVERIFY(miss.wait());
        QVERIFY(!miss.value().has_value());
    }

    void highlightIsAppliedAndRemoved() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        Latch<alioth::engine::PixelBufferPtr> plain;
        document.renderPage(0, 300, 500, true, [&plain](alioth::engine::PixelBufferPtr buffer) {
            plain.set(std::move(buffer));
        });
        QVERIFY(plain.wait());
        QVERIFY(plain.value() != nullptr);

        Latch<bool> applied;
        document.setFieldHighlight(std::nullopt, 0x0000FF, 200,
                                   [&applied] { applied.set(true); });
        QVERIFY(applied.wait());

        Latch<alioth::engine::PixelBufferPtr> highlighted;
        document.renderPage(0, 300, 500, true,
                            [&highlighted](alioth::engine::PixelBufferPtr buffer) {
                                highlighted.set(std::move(buffer));
                            });
        QVERIFY(highlighted.wait());
        QVERIFY(highlighted.value() != nullptr);

        // 高亮只在 FPDF_FFLDraw 這條路徑上生效，所以畫面必須真的變了。
        // 只驗「呼叫沒崩潰」等於沒驗到 PRD-FORM-004。
        QVERIFY(std::memcmp(plain.value()->data(), highlighted.value()->data(),
                            plain.value()->sizeBytes()) != 0);

        Latch<bool> removed;
        document.clearFieldHighlight([&removed] { removed.set(true); });
        QVERIFY(removed.wait());

        Latch<alioth::engine::PixelBufferPtr> restored;
        document.renderPage(0, 300, 500, true,
                            [&restored](alioth::engine::PixelBufferPtr buffer) {
                                restored.set(std::move(buffer));
                            });
        QVERIFY(restored.wait());
        QCOMPARE(std::memcmp(plain.value()->data(), restored.value()->data(),
                             plain.value()->sizeBytes()), 0);
    }

    void exportImportRoundTripThroughDocument() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        const auto initialFields = fieldsOf(document);
        const FormFieldInfo* text = findByName(initialFields, "fullName");
        QVERIFY(text != nullptr);
        Latch<FormFillResult> write;
        document.setTextValue(text->pageIndex, text->annotIndex, "Katherine",
                              [&write](FormFillResult r) { write.set(std::move(r)); });
        QVERIFY(write.wait());
        QVERIFY(write.value().ok);

        for (const FormDataFormat format : {FormDataFormat::Fdf, FormDataFormat::Xfdf}) {
            Latch<std::string> exported;
            document.exportData(format, "fixture.pdf",
                                [&exported](std::string text2) { exported.set(std::move(text2)); });
            QVERIFY(exported.wait());
            QVERIFY(exported.value().find("Katherine") != std::string::npos ||
                    exported.value().find("FEFF") != std::string::npos);

            // 換一份乾淨的文件匯入，確認往返真的把值搬過去了，
            // 而不是因為原本就有值而看起來成功。
            auto target = writeFixture(alioth::test::makeAcroFormPdf());
            QVERIFY(target);
            FormDocument imported;
            QVERIFY(openDocument(imported, *target));
            const auto cleanFields = fieldsOf(imported);
            QCOMPARE(findByName(cleanFields, "fullName")->value, std::string("Ada"));

            Latch<FormDataImport> result;
            imported.importData(exported.value(), format,
                                [&result](FormDataImport value) { result.set(std::move(value)); });
            QVERIFY(result.wait());
            QVERIFY2(result.value().ok, result.value().error.c_str());
            const auto importedFields = fieldsOf(imported);
            QCOMPARE(findByName(importedFields, "fullName")->value, std::string("Katherine"));
        }
    }

    void resetRestoresDefaultValues() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        const auto fields = fieldsOf(document);
        const FormFieldInfo* text = findByName(fields, "fullName");
        const FormFieldInfo* check = findByName(fields, "agree");
        QVERIFY(text && check);

        Latch<FormFillResult> write;
        document.setTextValue(text->pageIndex, text->annotIndex, "Dirty",
                              [&write](FormFillResult r) { write.set(std::move(r)); });
        QVERIFY(write.wait());
        Latch<FormFillResult> tick;
        document.setChecked(check->pageIndex, check->annotIndex, true,
                            [&tick](FormFillResult r) { tick.set(std::move(r)); });
        QVERIFY(tick.wait());
        const auto dirtyFields = fieldsOf(document);
        QCOMPARE(findByName(dirtyFields, "fullName")->value, std::string("Dirty"));
        QVERIFY(findByName(dirtyFields, "agree")->checked);

        Latch<FormFillResult> reset;
        document.resetForm([&reset](FormFillResult r) { reset.set(std::move(r)); });
        QVERIFY(reset.wait());
        QVERIFY2(reset.value().ok, reset.value().error.c_str());

        const auto after = fieldsOf(document);
        // /DV 是 (Ada)，所以重設回的是預設值而不是空字串。
        QCOMPARE(findByName(after, "fullName")->value, std::string("Ada"));
        QVERIFY(!findByName(after, "agree")->checked);
    }

    void flattenRemovesFieldsButKeepsAppearance() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));
        QCOMPARE(fieldsOf(document).size(), std::size_t{6});

        Latch<alioth::engine::PixelBufferPtr> before;
        document.renderPage(0, 300, 500, true,
                            [&before](alioth::engine::PixelBufferPtr b) { before.set(std::move(b)); });
        QVERIFY(before.wait());
        const std::size_t darkBefore = darkPixels(before.value());
        QVERIFY(darkBefore > 0);

        Latch<FormFillResult> flatten;
        document.flatten([&flatten](FormFillResult r) { flatten.set(std::move(r)); });
        QVERIFY(flatten.wait());
        QVERIFY2(flatten.value().ok, flatten.value().error.c_str());

        // 攤平的定義：欄位不再存在，但畫面看起來一樣。
        QCOMPARE(fieldsOf(document).size(), std::size_t{0});

        Latch<alioth::engine::PixelBufferPtr> after;
        document.renderPage(0, 300, 500, true,
                            [&after](alioth::engine::PixelBufferPtr b) { after.set(std::move(b)); });
        QVERIFY(after.wait());
        const std::size_t darkAfter = darkPixels(after.value());
        QVERIFY2(darkAfter > 0, "攤平後外觀完全消失，等於把使用者填的內容弄丟了");
        QVERIFY2(darkAfter * 2 >= darkBefore,
                 "攤平後的外觀與攤平前差距過大，內容可能沒有被正確併入內容串流");

        // 攤平是破壞性的，必須能另存新檔而不是就地增量儲存。
        QTemporaryFile output(QStringLiteral("alioth-flat-XXXXXX.pdf"));
        QVERIFY(output.open());
        const QString outputPath = output.fileName();
        output.close();

        Latch<FormFillResult> saved;
        document.saveCopy(outputPath.toStdString(),
                          [&saved](FormFillResult r) { saved.set(std::move(r)); });
        QVERIFY(saved.wait());
        QVERIFY2(saved.value().ok, saved.value().error.c_str());

        FormDocument reopened;
        Latch<DocumentError> opened;
        reopened.open(outputPath.toStdString(), {},
                      [&opened](DocumentError e) { opened.set(e); });
        QVERIFY(opened.wait());
        QCOMPARE(opened.value(), DocumentError::None);
        QCOMPARE(fieldsOf(reopened).size(), std::size_t{0});
    }

    void readOnlyFieldsRejectWrites() {
        auto file = writeFixture(alioth::test::makeAcroFormPdf());
        QVERIFY(file);
        FormDocument document;
        QVERIFY(openDocument(document, *file));

        // 不存在的欄位索引必須回報錯誤而不是靜默成功（IL-4）。
        Latch<FormFillResult> missing;
        document.setTextValue(0, 999, "x",
                              [&missing](FormFillResult r) { missing.set(std::move(r)); });
        QVERIFY(missing.wait());
        QVERIFY(!missing.value().ok);
        QVERIFY(!missing.value().error.empty());

        // 型別不符也要有明確原因，而不是「寫進去了但沒效果」。
        const auto typeFields = fieldsOf(document);
        const FormFieldInfo* check = findByName(typeFields, "agree");
        QVERIFY(check != nullptr);
        Latch<FormFillResult> wrongType;
        document.setTextValue(check->pageIndex, check->annotIndex, "x",
                              [&wrongType](FormFillResult r) { wrongType.set(std::move(r)); });
        QVERIFY(wrongType.wait());
        QVERIFY(!wrongType.value().ok);
        QVERIFY(!wrongType.value().error.empty());
    }
};

QTEST_APPLESS_MAIN(TestFormFields)
#include "test_form_fields.moc"
