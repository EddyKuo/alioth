#include "ui/annotation_properties_panel.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace alioth::ui {
namespace {

using alioth::domain::AnnotationFlag;
using alioth::domain::AnnotationType;
using alioth::domain::BorderStyleKind;
using alioth::domain::ColorRgb;

QString typeName(AnnotationType type) {
    switch (type) {
        case AnnotationType::Highlight: return QObject::tr("螢光筆");
        case AnnotationType::Underline: return QObject::tr("底線");
        case AnnotationType::StrikeOut: return QObject::tr("刪除線");
        case AnnotationType::Squiggly:  return QObject::tr("波浪線");
        case AnnotationType::Square:    return QObject::tr("矩形");
        case AnnotationType::Circle:    return QObject::tr("橢圓");
        case AnnotationType::Line:      return QObject::tr("線段");
        case AnnotationType::Ink:       return QObject::tr("手繪");
        case AnnotationType::Text:      return QObject::tr("便利貼");
        case AnnotationType::Caret:     return QObject::tr("校正符號");
        case AnnotationType::FreeText:  return QObject::tr("文字框");
        case AnnotationType::Polygon:   return QObject::tr("多邊形");
        case AnnotationType::PolyLine:  return QObject::tr("折線");
        case AnnotationType::Stamp:     return QObject::tr("圖章");
    }
    return QObject::tr("註解");
}

QColor toQColor(const ColorRgb& color) {
    return QColor::fromRgbF(color.r, color.g, color.b);
}

ColorRgb fromQColor(const QColor& color) {
    return ColorRgb{color.redF(), color.greenF(), color.blueF()};
}

// 色票按鈕的外觀。用背景色加上十六進位文字：顏色不單獨承載意義，
// 色盲使用者要能從文字知道自己選了什麼（與簽章面板同一條規則）。
void paintColorButton(QPushButton* button, const QColor& color) {
    // 顏色本身存在 property 裡，不從按鈕文字回推：文字是給人看的，
    // 拿它當資料來源會在本地化或格式微調時安靜地壞掉。
    button->setProperty("alioth_color", color);
    button->setText(color.name().toUpper());
    const bool dark = color.lightnessF() < 0.5;
    button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                              .arg(color.name(), dark ? QStringLiteral("#ffffff")
                                                      : QStringLiteral("#000000")));
}

}  // namespace

AnnotationPropertiesPanel::AnnotationPropertiesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("annotationPropertiesPanel"));
    setAccessibleName(tr("註解屬性"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    stack_ = new QStackedWidget(this);

    auto* empty = new QLabel(tr("選取一則註解以檢視並修改它的屬性。"), this);
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignTop);
    empty->setObjectName(QStringLiteral("annotationPropertiesEmpty"));

    auto* editor = new QWidget(this);
    auto* form = new QFormLayout(editor);

    typeLabel_ = new QLabel(editor);
    typeLabel_->setObjectName(QStringLiteral("annotationTypeLabel"));

    colorButton_ = new QPushButton(editor);
    colorButton_->setObjectName(QStringLiteral("annotationColorButton"));
    colorButton_->setAccessibleName(tr("主要顏色"));

    interiorEnabled_ = new QCheckBox(tr("填色"), editor);
    interiorEnabled_->setObjectName(QStringLiteral("annotationInteriorEnabled"));
    interiorButton_ = new QPushButton(editor);
    interiorButton_->setObjectName(QStringLiteral("annotationInteriorButton"));
    interiorButton_->setAccessibleName(tr("填滿顏色"));

    opacity_ = new QDoubleSpinBox(editor);
    opacity_->setObjectName(QStringLiteral("annotationOpacity"));
    opacity_->setRange(0.0, 1.0);
    opacity_->setSingleStep(0.05);
    opacity_->setDecimals(2);

    borderWidth_ = new QDoubleSpinBox(editor);
    borderWidth_->setObjectName(QStringLiteral("annotationBorderWidth"));
    // 下限是 0 而不是 0.1：PDF 規格裡線寬 0 代表「裝置能畫的最細線」，
    // 那是工程圖上讓細線永遠看得見的唯一合法手段（見
    // exceptions/EXC_20260906_RD_SA_thin_lines_unsupported.md）。
    borderWidth_->setRange(0.0, 60.0);
    borderWidth_->setSingleStep(0.5);
    borderWidth_->setDecimals(2);
    borderWidth_->setSpecialValueText(tr("最細（0）"));

    borderStyle_ = new QComboBox(editor);
    borderStyle_->setObjectName(QStringLiteral("annotationBorderStyle"));
    borderStyle_->addItem(tr("實線"), static_cast<int>(BorderStyleKind::Solid));
    borderStyle_->addItem(tr("虛線"), static_cast<int>(BorderStyleKind::Dashed));
    borderStyle_->addItem(tr("斜角"), static_cast<int>(BorderStyleKind::Beveled));
    borderStyle_->addItem(tr("內凹"), static_cast<int>(BorderStyleKind::Inset));
    borderStyle_->addItem(tr("底線"), static_cast<int>(BorderStyleKind::Underline));

    // 段落屬性（PRD-ANN-031）。行距是係數不是點數：字級改了之後行距要跟著變，
    // 存絕對點數會讓使用者每次改字級都要重調一次行距。
    lineSpacing_ = new QDoubleSpinBox(editor);
    lineSpacing_->setObjectName(QStringLiteral("annotationLineSpacing"));
    lineSpacing_->setRange(0.5, 4.0);
    lineSpacing_->setSingleStep(0.1);
    lineSpacing_->setDecimals(2);

    indent_ = new QDoubleSpinBox(editor);
    indent_->setObjectName(QStringLiteral("annotationIndent"));
    indent_->setRange(0.0, 200.0);
    indent_->setSingleStep(2.0);
    indent_->setDecimals(1);
    indent_->setSuffix(tr(" pt"));

    author_ = new QLineEdit(editor);
    author_->setObjectName(QStringLiteral("annotationAuthor"));
    subject_ = new QLineEdit(editor);
    subject_->setObjectName(QStringLiteral("annotationSubject"));
    contents_ = new QPlainTextEdit(editor);
    contents_->setObjectName(QStringLiteral("annotationContents"));
    contents_->setMaximumHeight(120);

    locked_ = new QCheckBox(tr("鎖定"), editor);
    locked_->setObjectName(QStringLiteral("annotationLocked"));
    hidden_ = new QCheckBox(tr("隱藏"), editor);
    hidden_->setObjectName(QStringLiteral("annotationHidden"));
    printable_ = new QCheckBox(tr("列印"), editor);
    printable_->setObjectName(QStringLiteral("annotationPrintable"));

    form->addRow(tr("類型"), typeLabel_);
    form->addRow(tr("顏色"), colorButton_);
    form->addRow(interiorEnabled_, interiorButton_);
    form->addRow(tr("不透明度"), opacity_);
    form->addRow(tr("線寬"), borderWidth_);
    form->addRow(tr("線型"), borderStyle_);
    form->addRow(tr("行距"), lineSpacing_);
    form->addRow(tr("縮排"), indent_);
    lineSpacingLabel_ = form->labelForField(lineSpacing_);
    indentLabel_ = form->labelForField(indent_);
    form->addRow(tr("作者"), author_);
    form->addRow(tr("主旨"), subject_);
    form->addRow(tr("內容"), contents_);
    form->addRow(locked_);
    form->addRow(hidden_);
    form->addRow(printable_);

    stack_->addWidget(empty);
    stack_->addWidget(editor);
    layout->addWidget(stack_);

    connect(colorButton_, &QPushButton::clicked, this, [this] {
        if (!current_.has_value()) return;
        const QColor picked = QColorDialog::getColor(toQColor(current_->color), this,
                                                     tr("選擇顏色"));
        if (!picked.isValid()) return;
        paintColorButton(colorButton_, picked);
        emitEdited();
    });
    connect(interiorButton_, &QPushButton::clicked, this, [this] {
        if (!current_.has_value()) return;
        const QColor initial = current_->interiorColor.has_value()
                                   ? toQColor(*current_->interiorColor)
                                   : QColor(Qt::white);
        const QColor picked = QColorDialog::getColor(initial, this, tr("選擇填色"));
        if (!picked.isValid()) return;
        paintColorButton(interiorButton_, picked);
        interiorEnabled_->setChecked(true);
        emitEdited();
    });

    connect(interiorEnabled_, &QCheckBox::toggled, this, [this](bool on) {
        interiorButton_->setEnabled(on);
        emitEdited();
    });
    connect(opacity_, &QDoubleSpinBox::valueChanged, this, [this] { emitEdited(); });
    connect(borderWidth_, &QDoubleSpinBox::valueChanged, this, [this] { emitEdited(); });
    connect(borderStyle_, &QComboBox::currentIndexChanged, this, [this] { emitEdited(); });
    connect(lineSpacing_, &QDoubleSpinBox::valueChanged, this, [this] { emitEdited(); });
    connect(indent_, &QDoubleSpinBox::valueChanged, this, [this] { emitEdited(); });
    connect(author_, &QLineEdit::editingFinished, this, [this] { emitEdited(); });
    connect(subject_, &QLineEdit::editingFinished, this, [this] { emitEdited(); });
    // 內容用失焦而不是 textChanged：每打一個字就送出一次的話，
    // 復原堆疊會被逐字元的命令灌爆。
    contents_->installEventFilter(this);
    connect(locked_, &QCheckBox::toggled, this, [this] { emitEdited(); });
    connect(hidden_, &QCheckBox::toggled, this, [this] { emitEdited(); });
    connect(printable_, &QCheckBox::toggled, this, [this] { emitEdited(); });

    setAnnotation(std::nullopt);
}

AnnotationPropertiesPanel::~AnnotationPropertiesPanel() = default;

bool AnnotationPropertiesPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == contents_ && event->type() == QEvent::FocusOut) emitEdited();
    return QWidget::eventFilter(watched, event);
}

void AnnotationPropertiesPanel::setAnnotation(
    const std::optional<alioth::domain::Annotation>& annotation) {
    current_ = annotation;
    rebuildFromCurrent();
}

void AnnotationPropertiesPanel::rebuildFromCurrent() {
    if (!current_.has_value()) {
        stack_->setCurrentIndex(0);
        return;
    }
    stack_->setCurrentIndex(1);

    // 由程式填值時抑制訊號，否則每次選取都會在復原堆疊裡多出一筆空命令。
    updating_ = true;

    const alioth::domain::Annotation& a = *current_;
    typeLabel_->setText(typeName(a.type()));
    paintColorButton(colorButton_, toQColor(a.color));
    interiorEnabled_->setChecked(a.interiorColor.has_value());
    interiorButton_->setEnabled(a.interiorColor.has_value());
    paintColorButton(interiorButton_,
                     a.interiorColor.has_value() ? toQColor(*a.interiorColor) : QColor(Qt::white));
    opacity_->setValue(a.opacity);
    borderWidth_->setValue(a.border.width);
    borderStyle_->setCurrentIndex(borderStyle_->findData(static_cast<int>(a.border.style)));
    author_->setText(QString::fromStdString(a.author));
    subject_->setText(QString::fromStdString(a.subject));
    contents_->setPlainText(QString::fromStdString(a.contents));
    // 段落屬性只對 FreeText 家族有意義。其他型別整列隱藏——灰掉的控制項
    // 會讓使用者一直在找怎麼啟用它。
    const auto* freeText = std::get_if<alioth::domain::FreeTextGeometry>(&a.geometry);
    const bool paragraphApplies = freeText != nullptr;
    lineSpacing_->setVisible(paragraphApplies);
    indent_->setVisible(paragraphApplies);
    if (lineSpacingLabel_ != nullptr) lineSpacingLabel_->setVisible(paragraphApplies);
    if (indentLabel_ != nullptr) indentLabel_->setVisible(paragraphApplies);
    if (paragraphApplies) {
        lineSpacing_->setValue(freeText->lineSpacing);
        indent_->setValue(freeText->indentPt);
    }

    locked_->setChecked(hasFlag(a.flags, AnnotationFlag::Locked));
    hidden_->setChecked(hasFlag(a.flags, AnnotationFlag::Hidden));
    printable_->setChecked(hasFlag(a.flags, AnnotationFlag::Print));

    updating_ = false;
}

alioth::domain::Annotation AnnotationPropertiesPanel::collect() const {
    alioth::domain::Annotation result = current_.value();

    result.color = fromQColor(colorButton_->property("alioth_color").value<QColor>());
    if (interiorEnabled_->isChecked()) {
        result.interiorColor =
            fromQColor(interiorButton_->property("alioth_color").value<QColor>());
    } else {
        result.interiorColor.reset();
    }
    result.opacity = opacity_->value();
    result.border.width = borderWidth_->value();
    result.border.style = static_cast<BorderStyleKind>(borderStyle_->currentData().toInt());
    if (auto* freeText = std::get_if<alioth::domain::FreeTextGeometry>(&result.geometry)) {
        freeText->lineSpacing = lineSpacing_->value();
        freeText->indentPt = indent_->value();
    }
    result.author = author_->text().toStdString();
    result.subject = subject_->text().toStdString();
    result.contents = contents_->toPlainText().toStdString();

    // 旗標是位元集合，逐位元設定而不是整份覆寫：/F 裡還有 NoZoom、NoRotate
    // 等我們不呈現的位元，整份覆寫會把它們清掉，而那是別的工具設的。
    auto flags = result.flags;
    const auto apply = [&flags](AnnotationFlag flag, bool on) {
        flags = on ? (flags | flag)
                   : static_cast<AnnotationFlag>(static_cast<std::uint32_t>(flags) &
                                                 ~static_cast<std::uint32_t>(flag));
    };
    apply(AnnotationFlag::Locked, locked_->isChecked());
    apply(AnnotationFlag::LockedContents, locked_->isChecked());
    apply(AnnotationFlag::Hidden, hidden_->isChecked());
    apply(AnnotationFlag::Print, printable_->isChecked());
    result.flags = flags;

    return result;
}

void AnnotationPropertiesPanel::emitEdited() {
    if (updating_ || !current_.has_value()) return;
    const alioth::domain::Annotation updated = collect();
    current_ = updated;
    emit annotationEdited(updated);
}

}  // namespace alioth::ui
