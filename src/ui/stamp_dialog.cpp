#include "ui/stamp_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include <initializer_list>

#include "domain/page_range.h"

namespace alioth::ui {
namespace {

using app::print::StampAnchor;

struct AnchorEntry {
    StampAnchor anchor;
    const char* label;
};

// 九宮格的順序照著畫面上的位置排，不是照 enum 的宣告順序——下拉選單裡
// 「左上、中上、右上…」讀起來就是一張圖，照 enum 排會變成一串亂序。
const AnchorEntry kAnchors[] = {
    {StampAnchor::TopLeft, QT_TRANSLATE_NOOP("StampDialog", "左上")},
    {StampAnchor::TopCenter, QT_TRANSLATE_NOOP("StampDialog", "中上")},
    {StampAnchor::TopRight, QT_TRANSLATE_NOOP("StampDialog", "右上")},
    {StampAnchor::MiddleLeft, QT_TRANSLATE_NOOP("StampDialog", "左中")},
    {StampAnchor::Center, QT_TRANSLATE_NOOP("StampDialog", "正中")},
    {StampAnchor::MiddleRight, QT_TRANSLATE_NOOP("StampDialog", "右中")},
    {StampAnchor::BottomLeft, QT_TRANSLATE_NOOP("StampDialog", "左下")},
    {StampAnchor::BottomCenter, QT_TRANSLATE_NOOP("StampDialog", "中下")},
    {StampAnchor::BottomRight, QT_TRANSLATE_NOOP("StampDialog", "右下")},
};

}  // namespace

StampDialog::StampDialog(Preset preset, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("stampDialog"));

    auto* form = new QFormLayout;

    text_ = new QLineEdit(this);
    text_->setObjectName(QStringLiteral("stampText"));
    form->addRow(tr("文字"), text_);

    auto* tokens = new QLabel(
        tr("可用符號：<<Page>> 頁碼、<<Pages>> 總頁數、<<Bates>> Bates 編號、"
           "<<Date>> 日期、<<File>> 檔名"),
        this);
    tokens->setWordWrap(true);
    form->addRow(QString(), tokens);

    pageRange_ = new QLineEdit(this);
    pageRange_->setObjectName(QStringLiteral("stampPageRange"));
    pageRange_->setPlaceholderText(tr("留空代表全部頁面；例如 1,3,5-8"));
    form->addRow(tr("頁面範圍"), pageRange_);

    anchor_ = new QComboBox(this);
    anchor_->setObjectName(QStringLiteral("stampAnchor"));
    for (const AnchorEntry& entry : kAnchors) {
        anchor_->addItem(tr(entry.label), static_cast<int>(entry.anchor));
    }
    form->addRow(tr("位置"), anchor_);

    fontSize_ = new QDoubleSpinBox(this);
    fontSize_->setObjectName(QStringLiteral("stampFontSize"));
    fontSize_->setRange(4.0, 144.0);
    fontSize_->setSuffix(tr(" pt"));
    form->addRow(tr("字級"), fontSize_);

    margin_ = new QDoubleSpinBox(this);
    margin_->setObjectName(QStringLiteral("stampMargin"));
    margin_->setRange(0.0, 200.0);
    margin_->setSuffix(tr(" pt"));
    form->addRow(tr("邊距"), margin_);

    auto* batesBox = new QGroupBox(tr("Bates 編號"), this);
    batesBox->setObjectName(QStringLiteral("stampBatesBox"));
    auto* batesForm = new QFormLayout(batesBox);

    useBates_ = new QCheckBox(tr("啟用（文字中請使用 <<Bates>>）"), batesBox);
    useBates_->setObjectName(QStringLiteral("stampUseBates"));
    batesForm->addRow(useBates_);

    batesPrefix_ = new QLineEdit(batesBox);
    batesPrefix_->setObjectName(QStringLiteral("stampBatesPrefix"));
    batesForm->addRow(tr("前綴"), batesPrefix_);

    batesSuffix_ = new QLineEdit(batesBox);
    batesSuffix_->setObjectName(QStringLiteral("stampBatesSuffix"));
    batesForm->addRow(tr("後綴"), batesSuffix_);

    batesStart_ = new QSpinBox(batesBox);
    batesStart_->setObjectName(QStringLiteral("stampBatesStart"));
    batesStart_->setRange(0, 1000000000);
    batesStart_->setValue(1);
    batesForm->addRow(tr("起始號"), batesStart_);

    batesDigits_ = new QSpinBox(batesBox);
    batesDigits_->setObjectName(QStringLiteral("stampBatesDigits"));
    // 上限來自 print::kMaxBatesDigits：再多只會產出沒有人讀得懂的字串，
    // 而且多半代表使用者把起始號填進了位數欄位。
    batesDigits_->setRange(0, app::print::kMaxBatesDigits);
    batesDigits_->setValue(6);
    batesForm->addRow(tr("補零位數"), batesDigits_);

    batesIncrement_ = new QSpinBox(batesBox);
    batesIncrement_->setObjectName(QStringLiteral("stampBatesIncrement"));
    // 負的遞增量是合法的：倒序編號的卷宗確實存在（見 BatesOptions::increment）。
    batesIncrement_->setRange(-1000, 1000);
    batesIncrement_->setValue(1);
    batesForm->addRow(tr("每頁遞增"), batesIncrement_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("stampButtons"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(useBates_, &QCheckBox::toggled, this, &StampDialog::updateBatesEnabled);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(batesBox);
    layout->addWidget(buttons);

    applyPreset(preset);
    updateBatesEnabled();
}

void StampDialog::applyPreset(Preset preset) {
    switch (preset) {
        case Preset::HeaderFooter:
            setWindowTitle(tr("頁首頁尾"));
            text_->setText(QStringLiteral("<<Page>> / <<Pages>>"));
            anchor_->setCurrentIndex(anchor_->findData(static_cast<int>(StampAnchor::BottomCenter)));
            fontSize_->setValue(10.0);
            margin_->setValue(36.0);
            break;
        case Preset::Watermark:
            setWindowTitle(tr("浮水印"));
            text_->setText(tr("機密"));
            anchor_->setCurrentIndex(anchor_->findData(static_cast<int>(StampAnchor::Center)));
            // 浮水印預設大字置中。目前的戳記通道不做旋轉與透明度——那需要
            // /ExtGState 與旋轉矩陣，屬於外觀產生器的範圍，不在這個入口。
            fontSize_->setValue(48.0);
            margin_->setValue(0.0);
            break;
        case Preset::Bates:
            setWindowTitle(tr("Bates 編號"));
            text_->setText(QStringLiteral("<<Bates>>"));
            anchor_->setCurrentIndex(anchor_->findData(static_cast<int>(StampAnchor::BottomRight)));
            fontSize_->setValue(10.0);
            margin_->setValue(24.0);
            useBates_->setChecked(true);
            break;
    }
}

void StampDialog::updateBatesEnabled() {
    const bool on = useBates_->isChecked();
    for (QWidget* widget : std::initializer_list<QWidget*>{
             batesPrefix_, batesSuffix_, batesStart_, batesDigits_, batesIncrement_}) {
        widget->setEnabled(on);
    }
}

bool StampDialog::pageRangeIsValid(int totalPages) const {
    const QString text = pageRange_->text().trimmed();
    if (text.isEmpty()) return true;  // 留空＝全部頁面
    return !domain::parsePageRange(text.toStdString(), totalPages).empty();
}

app::StampRequest StampDialog::request(int totalPages) const {
    app::StampRequest request;
    // 空的 pages 代表全部頁面（StampRequest 的既有語意），所以留空時
    // 不必特別處理。
    const QString range = pageRange_->text().trimmed();
    if (!range.isEmpty()) {
        for (const int page : domain::parsePageRange(range.toStdString(), totalPages)) {
            request.pages.push_back(page);
        }
    }
    request.textTemplate = text_->text();
    request.anchor = static_cast<StampAnchor>(anchor_->currentData().toInt());
    request.fontSize = fontSize_->value();

    const double margin = margin_->value();
    request.margins = app::print::StampMargins{margin, margin, margin, margin};

    request.useBates = useBates_->isChecked();
    request.bates.enabled = request.useBates;
    request.bates.prefix = batesPrefix_->text();
    request.bates.suffix = batesSuffix_->text();
    request.bates.startNumber = batesStart_->value();
    request.bates.digits = batesDigits_->value();
    request.bates.increment = batesIncrement_->value();
    // 寫進文件時序號跟著文件頁碼走，不是列印張數——法務要的是「這一頁是
    // 第幾號」，而列印張數會因為雙面或 N-up 而與頁碼脫鉤。
    request.bates.basis = app::print::BatesBasis::DocumentPage;
    return request;
}

}  // namespace alioth::ui
