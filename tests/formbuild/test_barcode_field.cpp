// 條碼欄位（PRD-FORM-025，WP35）。

#include <QtTest>

#include "engine/formbuild/field_appearance.h"
#include "engine/formbuild/field_definition.h"

using namespace alioth;
using namespace alioth::engine::formbuild;

namespace {

FieldDefinition barcodeField(std::string value, domain::RectF rect = domain::RectF{0.0, 0.0, 200.0, 60.0}) {
    FieldDefinition definition;
    definition.type = BuildFieldType::Barcode;
    definition.name = "field.barcode";
    definition.rectPt = rect;
    definition.value = std::move(value);
    return definition;
}

}  // namespace

class TestBarcodeField : public QObject {
    Q_OBJECT

private slots:
    void fieldTypeMapsToTextFieldType() {
        QCOMPARE(std::string(fieldTypeName(BuildFieldType::Barcode)), std::string("Tx"));
    }

    void barcodeFieldIsAlwaysReadOnly() {
        const FieldDefinition definition = barcodeField("ABC123");
        const std::int64_t flags = computeFieldFlags(definition);
        QVERIFY2((flags & (std::int64_t{1} << 0)) != 0, "條碼欄位必須是唯讀（避免手改文字與 /V 不同步）");
    }

    void emptyValueFailsValidation() {
        const FieldDefinition definition = barcodeField("");
        QVERIFY(!validate(definition).empty());
    }

    void nonAsciiValueFailsValidation() {
        const FieldDefinition definition = barcodeField("\xE4\xB8\xAD\xE6\x96\x87");
        QVERIFY(!validate(definition).empty());
    }

    void validValueProducesAppearanceWithFilledRects() {
        const FieldDefinition definition = barcodeField("ORDER-42");
        const FieldAppearance appearance = generateFieldAppearance(definition);
        QVERIFY2(appearance.valid, appearance.diagnostic.c_str());
        QVERIFY(!appearance.states.empty());
        QVERIFY2(appearance.states.front().content.find(" re\n") != std::string::npos,
                "沒有畫出任何條碼矩形");
        QVERIFY2(appearance.states.front().content.find("f\n") != std::string::npos,
                "沒有填色指令");
    }

    void tooShortRectFailsAppearanceGeneration() {
        // 靜區是寬度的比例，理論上再窄都還會留下一絲寬度；但底部留白
        // （kTextPadding）是固定點數，矩形矮到連這個固定值都放不下時，
        // 有墨區域的上下界會反轉，這是唯一保證能觸發「畫不出來」的情況。
        const FieldDefinition definition = barcodeField("ABC", domain::RectF{0.0, 0.0, 200.0, 1.0});
        const FieldAppearance appearance = generateFieldAppearance(definition);
        QVERIFY(!appearance.valid);
    }
};

QTEST_MAIN(TestBarcodeField)
#include "test_barcode_field.moc"
