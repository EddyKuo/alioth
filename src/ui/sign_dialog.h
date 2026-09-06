#pragma once

// 數位簽署對話框（PRD-SIG-004）。
//
// 只收簽署所需的最小資訊：憑證檔、密碼、以及 /Reason /Location /ContactInfo
// 三個會寫進簽章字典的欄位。
//
// **密碼不留任何痕跡**：不記進 QSettings、不寫進 log、對話框關閉時就消失。
// 私鑰密碼一旦落到磁碟或訊息裡，就等於私鑰本身外洩，而那個外洩不會有任何
// 徵兆。輸入框走 QLineEdit::Password，連肩窺都擋掉。

#include <QDialog>

#include "app/signature_controller.h"

class QLineEdit;

namespace alioth::ui {

class SignDialog : public QDialog {
    Q_OBJECT

public:
    explicit SignDialog(QWidget* parent = nullptr);

    [[nodiscard]] app::SignatureController::SigningRequest request() const;

private:
    void browseForCertificate();
    void updateOkButton();

    QLineEdit* certificatePath_{nullptr};
    QLineEdit* password_{nullptr};
    QLineEdit* reason_{nullptr};
    QLineEdit* location_{nullptr};
    QLineEdit* contact_{nullptr};
    QLineEdit* timestampUrl_{nullptr};
    class QPushButton* okButton_{nullptr};
};

}  // namespace alioth::ui
