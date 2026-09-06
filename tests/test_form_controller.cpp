// 表單控制器（PRD-FORM-021 的資料來源）。
//
// 這裡驗的是型別轉換與同名合併——那是唯一有判斷的地方。非同步開檔與
// 執行緒編組由 engine::forms::FormDocument 自己的測試涵蓋。

#include <QtTest>

#include "app/form_controller.h"

using namespace alioth;
using engine::formbuild::BuildFieldType;
using engine::forms::FormFieldInfo;
using engine::forms::FormFieldType;

namespace {

FormFieldInfo make(const std::string& name, FormFieldType type, std::int32_t page) {
    FormFieldInfo info;
    info.name = name;
    info.type = type;
    info.pageIndex = page;
    return info;
}

}  // namespace

class TestFormController : public QObject {
    Q_OBJECT

private slots:
    void typesMapToPanelTypes() {
        const std::vector<FormFieldInfo> fields{
            make("a", FormFieldType::TextField, 0), make("b", FormFieldType::CheckBox, 0),
            make("c", FormFieldType::ComboBox, 0),  make("d", FormFieldType::ListBox, 0),
            make("e", FormFieldType::PushButton, 0), make("f", FormFieldType::Signature, 0),
            make("g", FormFieldType::RadioButton, 0),
        };
        const auto summaries = app::summarizeFormFields(fields);
        QCOMPARE(summaries.size(), std::size_t{7});
        QCOMPARE(summaries[0].type, BuildFieldType::Text);
        QCOMPARE(summaries[1].type, BuildFieldType::CheckBox);
        QCOMPARE(summaries[2].type, BuildFieldType::ComboBox);
        QCOMPARE(summaries[3].type, BuildFieldType::ListBox);
        QCOMPARE(summaries[4].type, BuildFieldType::PushButton);
        QCOMPARE(summaries[5].type, BuildFieldType::Signature);
        QCOMPARE(summaries[6].type, BuildFieldType::RadioGroup);
    }

    void unknownTypeIsShownAsTextNotHidden() {
        // 面板漏列一個欄位，使用者會以為文件裡沒有那一格。
        const auto summaries = app::summarizeFormFields({make("x", FormFieldType::Unknown, 0)});
        QCOMPARE(summaries.size(), std::size_t{1});
        QCOMPARE(summaries[0].type, BuildFieldType::Text);
    }

    void sameNamedWidgetsCollapseIntoOneRow() {
        // 單選群組的每個按鈕都是獨立 widget 但共用欄位名。逐個列出會變成
        // 三個一模一樣的「性別」。
        const std::vector<FormFieldInfo> fields{
            make("gender", FormFieldType::RadioButton, 2),
            make("gender", FormFieldType::RadioButton, 1),
            make("gender", FormFieldType::RadioButton, 3),
            make("name", FormFieldType::TextField, 0),
        };
        const auto summaries = app::summarizeFormFields(fields);
        QCOMPARE(summaries.size(), std::size_t{2});
        QCOMPARE(summaries[0].name, std::string{"gender"});
        QCOMPARE(summaries[0].widgetCount, 3);
        // 頁碼取最小的：面板的「頁」是拿來跳過去的，跳到群組的第一個按鈕才對。
        QCOMPARE(summaries[0].pageIndex, 1);
        QCOMPARE(summaries[1].widgetCount, 1);
    }

    void flagsAreCarriedThrough() {
        FormFieldInfo info = make("locked", FormFieldType::TextField, 0);
        info.flags.readOnly = true;
        info.flags.required = true;
        const auto summaries = app::summarizeFormFields({info});
        QVERIFY(summaries[0].readOnly);
        QVERIFY(summaries[0].required);
    }

    void emptyInputYieldsEmptyOutput() {
        QVERIFY(app::summarizeFormFields({}).empty());
    }

    void orderFollowsInput() {
        // 面板需要可預期的順序（剛建立的欄位出現在最後），因此這裡不排序。
        const std::vector<FormFieldInfo> fields{make("z", FormFieldType::TextField, 0),
                                                make("a", FormFieldType::TextField, 0)};
        const auto summaries = app::summarizeFormFields(fields);
        QCOMPARE(summaries[0].name, std::string{"z"});
        QCOMPARE(summaries[1].name, std::string{"a"});
    }
};

QTEST_MAIN(TestFormController)
#include "test_form_controller.moc"
