// Email 表單資料（PRD-FORM-024）。
//
// 絕對不可在測試裡真的呼叫使用者的郵件用戶端，所以一律注入假的 sender，
// 只驗證「請求組得對不對」與「暫存附件檔案真的寫出去了」。

#include <QtTest>

#include <QDir>
#include <QFile>

#include "app/form_email.h"

using namespace alioth::app;
using namespace alioth::platform;

class TestFormEmail : public QObject {
    Q_OBJECT

private slots:
    void writesAttachmentAndInvokesSenderWithExpectedRequest() {
        FormEmailRequest request;
        request.exportedText = QStringLiteral("<?xml version=\"1.0\"?><xfdf></xfdf>");
        request.attachmentFileName = QStringLiteral("test-form-email.xfdf");
        request.to = QStringLiteral("reviewer@example.com");
        request.subject = QStringLiteral("表單資料");
        request.body = QStringLiteral("請查收附件");

        bool senderCalled = false;
        QString capturedAttachmentPath;
        EmailComposeRequest captured;
        EmailSender fakeSender = [&](const EmailComposeRequest& composeRequest) {
            senderCalled = true;
            captured = composeRequest;
            capturedAttachmentPath = composeRequest.attachmentPath;
            return EmailComposeResult{true, {}};
        };

        const EmailComposeResult result = sendFormDataByEmail(request, fakeSender);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(senderCalled);
        QCOMPARE(captured.to, request.to);
        QCOMPARE(captured.subject, request.subject);
        QCOMPARE(captured.body, request.body);

        QVERIFY(!capturedAttachmentPath.isEmpty());
        QVERIFY2(QFile::exists(capturedAttachmentPath),
                 qPrintable(QStringLiteral("附件檔案不存在：%1").arg(capturedAttachmentPath)));

        QFile file(capturedAttachmentPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), request.exportedText);
    }

    void senderFailureIsPropagatedNotSwallowed() {
        FormEmailRequest request;
        request.exportedText = QStringLiteral("data");
        request.attachmentFileName = QStringLiteral("fail-case.xfdf");

        EmailSender failingSender = [](const EmailComposeRequest&) {
            return EmailComposeResult{false, QStringLiteral("模擬找不到郵件用戶端")};
        };

        const EmailComposeResult result = sendFormDataByEmail(request, failingSender);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
    }

    void defaultAttachmentNameIsUsedWhenEmpty() {
        FormEmailRequest request;
        request.exportedText = QStringLiteral("data");
        // attachmentFileName 留空。

        QString capturedPath;
        EmailSender fakeSender = [&](const EmailComposeRequest& composeRequest) {
            capturedPath = composeRequest.attachmentPath;
            return EmailComposeResult{true, {}};
        };

        const EmailComposeResult result = sendFormDataByEmail(request, fakeSender);
        QVERIFY(result.ok);
        QVERIFY(capturedPath.endsWith(QStringLiteral(".xfdf")));
    }
};

QTEST_MAIN(TestFormEmail)
#include "test_form_email.moc"
