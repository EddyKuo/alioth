#include "ui/sign_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace alioth::ui {

SignDialog::SignDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("數位簽署"));
    setObjectName(QStringLiteral("signDialog"));

    auto* form = new QFormLayout;

    auto* certificateRow = new QWidget(this);
    auto* certificateLayout = new QHBoxLayout(certificateRow);
    certificateLayout->setContentsMargins(0, 0, 0, 0);
    certificatePath_ = new QLineEdit(certificateRow);
    certificatePath_->setObjectName(QStringLiteral("signCertificatePath"));
    certificatePath_->setPlaceholderText(tr("PKCS#12 憑證（.pfx / .p12）"));
    auto* browse = new QPushButton(tr("瀏覽…"), certificateRow);
    browse->setObjectName(QStringLiteral("signBrowse"));
    certificateLayout->addWidget(certificatePath_, 1);
    certificateLayout->addWidget(browse);
    form->addRow(tr("簽署憑證"), certificateRow);

    password_ = new QLineEdit(this);
    password_->setObjectName(QStringLiteral("signPassword"));
    // 密碼一律遮蔽，且不提供「顯示密碼」的眼睛按鈕：這個欄位只會被輸入一次，
    // 而肩窺是簽署情境最現實的威脅。
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("憑證密碼"), password_);

    reason_ = new QLineEdit(this);
    reason_->setObjectName(QStringLiteral("signReason"));
    reason_->setPlaceholderText(tr("例如：我已審閱並同意"));
    form->addRow(tr("簽署原因"), reason_);

    location_ = new QLineEdit(this);
    location_->setObjectName(QStringLiteral("signLocation"));
    form->addRow(tr("地點"), location_);

    contact_ = new QLineEdit(this);
    contact_->setObjectName(QStringLiteral("signContact"));
    form->addRow(tr("聯絡方式"), contact_);

    timestampUrl_ = new QLineEdit(this);
    timestampUrl_->setObjectName(QStringLiteral("signTimestampUrl"));
    timestampUrl_->setPlaceholderText(tr("留空（尚未支援）"));
    form->addRow(tr("時間戳伺服器"), timestampUrl_);

    auto* hint = new QLabel(
        tr("簽署以增量方式寫入檔案末端，原有內容一個位元組都不會被改動——"
           "文件裡既有的簽章因此仍然有效。"),
        this);
    hint->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("signButtons"));
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    okButton_->setText(tr("簽署"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, &SignDialog::browseForCertificate);
    connect(certificatePath_, &QLineEdit::textChanged, this, &SignDialog::updateOkButton);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    updateOkButton();
}

void SignDialog::browseForCertificate() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("選擇簽署憑證"), QString(),
        tr("PKCS#12 憑證 (*.pfx *.p12);;所有檔案 (*)"));
    if (!path.isEmpty()) certificatePath_->setText(path);
}

void SignDialog::updateOkButton() {
    // 密碼可以是空的（有些憑證沒設密碼），憑證路徑不能。
    okButton_->setEnabled(!certificatePath_->text().trimmed().isEmpty());
}

app::SignatureController::SigningRequest SignDialog::request() const {
    app::SignatureController::SigningRequest request;
    request.pkcs12Path = certificatePath_->text().trimmed();
    // 密碼不 trim：前後空白可能是密碼的一部分，替使用者「修正」會讓一個
    // 正確的密碼被判成錯的，而錯誤訊息刻意不區分密碼錯與檔案壞。
    request.password = password_->text();
    request.reason = reason_->text().trimmed();
    request.location = location_->text().trimmed();
    request.contactInfo = contact_->text().trimmed();
    request.timestampUrl = timestampUrl_->text().trimmed();
    return request;
}

}  // namespace alioth::ui
