// 簽署對話框的欄位狀態（PRD-SIG-004、PRD-SIG-006）。
//
// 這支測試守的是一條介面誠實性規則：**做不到的功能不可以長得像做得到**。
//
// 時間戳（TSA）的底層 RFC 3161 元件已經有了，但應用層的傳輸與連網同意流程
// 還沒接上，controller 對任何非空的時間戳網址都直接失敗。欄位曾經是可輸入的，
// 於是使用者填完整份簽署表單、按下「簽署」之後才會被告知不支援——那個成本
// 應該落在按下之前。
//
// 兩件事一起守：欄位停用（不可輸入），以及 controller 的拒絕行為仍然存在。
// 只守其中一個都不夠：只守 UI 的話，某天欄位被重新啟用而 controller 照樣失敗；
// 只守 controller 的話，UI 可以一直假裝這個功能能用。

#include <QtTest>

#include <QLineEdit>

#include "app/signature_controller.h"
#include "ui/sign_dialog.h"

using alioth::app::SignatureController;
using alioth::ui::SignDialog;

class TestSignDialog : public QObject {
    Q_OBJECT

private slots:
    void timestampFieldIsDisabledWhileUnsupported() {
        SignDialog dialog;
        auto* field = dialog.findChild<QLineEdit*>(QStringLiteral("signTimestampUrl"));
        QVERIFY2(field, "找不到時間戳欄位");
        QVERIFY2(!field->isEnabled(),
                 "時間戳欄位可以輸入，但 controller 對任何非空值都會失敗");
        QVERIFY2(!field->placeholderText().isEmpty(),
                 "停用的欄位必須說明為什麼不能用，否則看起來只是壞掉");
        QVERIFY2(field->text().isEmpty(), "停用的欄位不該預填內容");
    }

    // 停用欄位的結果是 request 裡的時間戳網址必定為空，簽署因此走得下去。
    void requestCarriesNoTimestampUrl() {
        SignDialog dialog;
        QCOMPARE(dialog.request().timestampUrl, QString());
    }

    // controller 的拒絕行為必須留著：欄位停用只是把成本提前，不是替代品。
    // 別的入口（批次簽署、巨集、日後的命令列）不會經過這個對話框。
    void controllerStillRefusesATimestampRequest() {
        SignatureController controller;
        SignatureController::SigningRequest request;
        request.pkcs12Path = QStringLiteral("unused.p12");
        request.timestampUrl = QStringLiteral("http://tsa.example.invalid/");
        const auto outcome = controller.signDocument(QStringLiteral("unused.pdf"), request);
        QVERIFY2(!outcome.ok, "要求時間戳卻沒有明確失敗");
        QVERIFY(!outcome.message.isEmpty());
    }
};

QTEST_MAIN(TestSignDialog)
#include "test_sign_dialog.moc"
