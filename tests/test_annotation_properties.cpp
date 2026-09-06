// 註解屬性側邊欄（PRD-ANN-009）。
//
// 這個面板最容易寫錯的兩件事，剛好也是最難用肉眼發現的：
//   1. 由程式填值時忘了抑制訊號 → 每次選取都在復原堆疊裡多一筆什麼都沒改的命令
//   2. 旗標整份覆寫 → 把別的工具設的 NoZoom / NoRotate 清掉，且沒有任何徵兆
// 因此兩者都以測試釘住。

#include <QtTest>

// findChild 取回的型別必須是完整型別才能呼叫成員函式。面板的標頭只做前置宣告
// （那是對的——標頭不該把 QtWidgets 拖進每個引用它的翻譯單元），
// 所以完整標頭由測試自己引入。
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>

#include "ui/annotation_properties_panel.h"

using namespace alioth;
using domain::AnnotationFlag;

namespace {

domain::Annotation sample() {
    domain::Annotation annotation;
    annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Square};
    annotation.rect = domain::RectF{10.0, 10.0, 110.0, 60.0};
    annotation.color = domain::ColorRgb{1.0, 0.0, 0.0};
    annotation.opacity = 0.8;
    annotation.border.width = 2.0;
    annotation.author = "Reviewer";
    annotation.subject = "檢討";
    annotation.contents = "這裡要改";
    // NoZoom 與 NoRotate 是面板不呈現的旗標，用來驗證它們不會被覆寫掉。
    annotation.flags = AnnotationFlag::Print | AnnotationFlag::NoZoom | AnnotationFlag::NoRotate;
    return annotation;
}

}  // namespace

class TestAnnotationProperties : public QObject {
    Q_OBJECT

private slots:
    void emptyStateWhenNothingSelected() {
        ui::AnnotationPropertiesPanel panel;
        QVERIFY(!panel.hasAnnotation());
        // 空狀態要有可見的說明，不是一排灰掉的控制項——後者看起來像壞了。
        QLabel* empty = panel.findChild<QLabel*>(QStringLiteral("annotationPropertiesEmpty"));
        QVERIFY(empty != nullptr);
        QVERIFY(!empty->text().isEmpty());
    }

    void selectingDoesNotEmitAnEdit() {
        // 由程式填值時若沒抑制訊號，每次選取都會產生一筆空命令。
        ui::AnnotationPropertiesPanel panel;
        QSignalSpy spy(&panel, &ui::AnnotationPropertiesPanel::annotationEdited);
        panel.setAnnotation(sample());
        QVERIFY(panel.hasAnnotation());
        QCOMPARE(spy.count(), 0);
    }

    void fieldsReflectTheSelectedAnnotation() {
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());

        QCOMPARE(panel.findChild<QLineEdit*>(QStringLiteral("annotationAuthor"))->text(),
                 QStringLiteral("Reviewer"));
        QCOMPARE(panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationOpacity"))->value(),
                 0.8);
        QCOMPARE(panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationBorderWidth"))->value(),
                 2.0);
        QVERIFY(panel.findChild<QCheckBox*>(QStringLiteral("annotationPrintable"))->isChecked());
        QVERIFY(!panel.findChild<QCheckBox*>(QStringLiteral("annotationLocked"))->isChecked());
    }

    void changingOpacityEmitsFullState() {
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());
        QSignalSpy spy(&panel, &ui::AnnotationPropertiesPanel::annotationEdited);

        panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationOpacity"))->setValue(0.4);
        QCOMPARE(spy.count(), 1);

        const auto updated = spy.at(0).at(0).value<domain::Annotation>();
        QCOMPARE(updated.opacity, 0.4);
        // 帶出的是完整狀態而不是差異：呼叫端要包成命令，需要完整快照才能復原。
        QCOMPARE(updated.author, std::string{"Reviewer"});
        QCOMPARE(updated.border.width, 2.0);
    }

    void unrelatedFlagsSurviveAnEdit() {
        // /F 裡還有面板不呈現的位元，整份覆寫會把別的工具設的清掉，
        // 而且沒有任何徵兆。
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());
        QSignalSpy spy(&panel, &ui::AnnotationPropertiesPanel::annotationEdited);

        panel.findChild<QCheckBox*>(QStringLiteral("annotationLocked"))->setChecked(true);
        QVERIFY(spy.count() >= 1);

        const auto updated = spy.at(spy.count() - 1).at(0).value<domain::Annotation>();
        QVERIFY(hasFlag(updated.flags, AnnotationFlag::Locked));
        // 鎖定要連同 LockedContents 一起設，否則內容還是改得動。
        QVERIFY(hasFlag(updated.flags, AnnotationFlag::LockedContents));
        QVERIFY(hasFlag(updated.flags, AnnotationFlag::NoZoom));
        QVERIFY(hasFlag(updated.flags, AnnotationFlag::NoRotate));
        QVERIFY(hasFlag(updated.flags, AnnotationFlag::Print));
    }

    void hairlineWidthIsSelectable() {
        // PDF 的線寬 0 代表「裝置能畫的最細線」，是工程圖上讓細線永遠看得見的
        // 唯一合法手段（見 exceptions/EXC_20260906_RD_SA_thin_lines_unsupported.md）。
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());
        auto* width = panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationBorderWidth"));
        QCOMPARE(width->minimum(), 0.0);

        QSignalSpy spy(&panel, &ui::AnnotationPropertiesPanel::annotationEdited);
        width->setValue(0.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<domain::Annotation>().border.width, 0.0);
    }

    void interiorColourCanBeClearedAndRestored() {
        domain::Annotation annotation = sample();
        annotation.interiorColor = domain::ColorRgb{0.0, 0.0, 1.0};

        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(annotation);
        auto* enabled = panel.findChild<QCheckBox*>(QStringLiteral("annotationInteriorEnabled"));
        QVERIFY(enabled->isChecked());

        QSignalSpy spy(&panel, &ui::AnnotationPropertiesPanel::annotationEdited);
        enabled->setChecked(false);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!spy.at(0).at(0).value<domain::Annotation>().interiorColor.has_value());
    }

    void switchingSelectionReplacesRatherThanMerges() {
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());

        domain::Annotation other;
        other.geometry = domain::TextMarkupGeometry{};
        other.author = "Someone Else";
        other.opacity = 0.25;
        panel.setAnnotation(other);

        QCOMPARE(panel.findChild<QLineEdit*>(QStringLiteral("annotationAuthor"))->text(),
                 QStringLiteral("Someone Else"));
        QCOMPARE(panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationOpacity"))->value(),
                 0.25);
    }

    void paragraphControlsAppearOnlyForFreeText() {
        // 灰掉的控制項會讓使用者一直在找怎麼啟用它；整列隱藏才對。
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());  // Square
        auto* spacing = panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationLineSpacing"));
        QVERIFY(spacing != nullptr);
        QVERIFY(!spacing->isVisibleTo(&panel));

        domain::Annotation freeText;
        domain::FreeTextGeometry geometry;
        geometry.lineSpacing = 1.8;
        geometry.indentPt = 12.0;
        freeText.geometry = geometry;
        freeText.rect = domain::RectF{0.0, 0.0, 200.0, 80.0};
        panel.setAnnotation(freeText);
        QVERIFY(spacing->isVisibleTo(&panel));
        QCOMPARE(spacing->value(), 1.8);
        QCOMPARE(panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationIndent"))->value(),
                 12.0);
    }

    void paragraphEditsReachTheGeometry() {
        domain::Annotation freeText;
        freeText.geometry = domain::FreeTextGeometry{};
        freeText.rect = domain::RectF{0.0, 0.0, 200.0, 80.0};

        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(freeText);
        QSignalSpy spy(&panel, &ui::AnnotationPropertiesPanel::annotationEdited);

        panel.findChild<QDoubleSpinBox*>(QStringLiteral("annotationIndent"))->setValue(24.0);
        QVERIFY(spy.count() >= 1);
        const auto updated = spy.at(spy.count() - 1).at(0).value<domain::Annotation>();
        const auto* geometry = std::get_if<domain::FreeTextGeometry>(&updated.geometry);
        QVERIFY(geometry != nullptr);
        QCOMPARE(geometry->indentPt, 24.0);
    }

    void deselectingReturnsToEmptyState() {
        ui::AnnotationPropertiesPanel panel;
        panel.setAnnotation(sample());
        QVERIFY(panel.hasAnnotation());
        panel.setAnnotation(std::nullopt);
        QVERIFY(!panel.hasAnnotation());
    }
};

QTEST_MAIN(TestAnnotationProperties)
#include "test_annotation_properties.moc"
