// 複製欄位（PRD-FORM-011~020）。

#include <QtTest>

#include "engine/formbuild/field_definition.h"

using namespace alioth;
using namespace alioth::engine::formbuild;

class TestFieldDuplicate : public QObject {
    Q_OBJECT

private slots:
    void duplicateOffsetsRectAndRenamesField() {
        FieldDefinition source;
        source.type = BuildFieldType::Text;
        source.name = "field.original";
        source.rectPt = domain::RectF{10.0, 10.0, 110.0, 34.0};
        source.value = "hello";

        const FieldDefinition copy = duplicateFieldDefinition(source, "field.copy");

        QCOMPARE(QString::fromStdString(copy.name), QStringLiteral("field.copy"));
        QCOMPARE(QString::fromStdString(source.name), QStringLiteral("field.original"));
        // 值與其他屬性原封不動地帶過去。
        QCOMPARE(QString::fromStdString(copy.value), QStringLiteral("hello"));
        // 預設位移後矩形不再與原欄位完全重疊，否則使用者看不出多了一個欄位。
        QVERIFY(copy.rectPt.left != source.rectPt.left || copy.rectPt.bottom != source.rectPt.bottom);
        // 寬高不變：位移是平移，不是縮放。
        QCOMPARE(copy.rectPt.width(), source.rectPt.width());
        QCOMPARE(copy.rectPt.height(), source.rectPt.height());
    }

    void duplicateWithExplicitOffset() {
        FieldDefinition source;
        source.type = BuildFieldType::Text;
        source.name = "a";
        source.rectPt = domain::RectF{0.0, 0.0, 50.0, 20.0};

        const FieldDefinition copy = duplicateFieldDefinition(source, "b", 100.0, 0.0);
        QCOMPARE(copy.rectPt.left, 100.0);
        QCOMPARE(copy.rectPt.right, 150.0);
        QCOMPARE(copy.rectPt.bottom, 0.0);
    }

    void duplicateRadioGroupOffsetsEveryButton() {
        FieldDefinition source;
        source.type = BuildFieldType::RadioGroup;
        source.name = "group.a";
        source.radios.push_back(RadioOption{"Yes", domain::RectF{0.0, 0.0, 20.0, 20.0}});
        source.radios.push_back(RadioOption{"No", domain::RectF{30.0, 0.0, 50.0, 20.0}});

        const FieldDefinition copy = duplicateFieldDefinition(source, "group.b", 5.0, 5.0);
        QCOMPARE(copy.radios.size(), source.radios.size());
        for (std::size_t i = 0; i < copy.radios.size(); ++i) {
            QCOMPARE(copy.radios[i].rectPt.left, source.radios[i].rectPt.left + 5.0);
            QCOMPARE(copy.radios[i].rectPt.bottom, source.radios[i].rectPt.bottom + 5.0);
            // 匯出值不變，否則複製出來的群組會跟原本的群組共用選項名。
            QCOMPARE(QString::fromStdString(copy.radios[i].exportValue),
                     QString::fromStdString(source.radios[i].exportValue));
        }
    }
};

QTEST_MAIN(TestFieldDuplicate)
#include "test_field_duplicate.moc"
