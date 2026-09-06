// Email 整份文件（PRD-IO-011）。
//
// 與 test_form_email.cpp 同樣的紀律：絕不真的呼叫使用者的郵件用戶端，
// 一律注入假的 sender，只驗證「請求組得對不對」。

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "app/document_email.h"

using namespace alioth::app;
using namespace alioth::platform;

class TestDocumentEmail : public QObject {
    Q_OBJECT

private slots:
    void attachesExistingDocumentAndInvokesSenderWithExpectedRequest() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString docPath = dir.filePath(QStringLiteral("report.pdf"));
        QFile file(docPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.7 fake");
        file.close();

        DocumentEmailRequest request;
        request.documentPath = docPath;
        request.to = QStringLiteral("reviewer@example.com");
        request.subject = QStringLiteral("審閱結果");
        request.body = QStringLiteral("請查收附件的 PDF");

        bool senderCalled = false;
        EmailComposeRequest captured;
        EmailSender fakeSender = [&](const EmailComposeRequest& composeRequest) {
            senderCalled = true;
            captured = composeRequest;
            return EmailComposeResult{true, {}};
        };

        const EmailComposeResult result = sendDocumentByEmail(request, fakeSender);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(senderCalled);
        QCOMPARE(captured.to, request.to);
        QCOMPARE(captured.subject, request.subject);
        QCOMPARE(captured.body, request.body);
        QCOMPARE(captured.attachmentPath, docPath);
    }

    void defaultSubjectUsesFileNameWhenEmpty() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString docPath = dir.filePath(QStringLiteral("contract-final.pdf"));
        QFile file(docPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.7 fake");
        file.close();

        DocumentEmailRequest request;
        request.documentPath = docPath;
        // subject 留空。

        QString capturedSubject;
        EmailSender fakeSender = [&](const EmailComposeRequest& composeRequest) {
            capturedSubject = composeRequest.subject;
            return EmailComposeResult{true, {}};
        };

        const EmailComposeResult result = sendDocumentByEmail(request, fakeSender);
        QVERIFY(result.ok);
        QVERIFY(capturedSubject.contains(QStringLiteral("contract-final.pdf")));
    }

    void emptyDocumentPathFailsBeforeCallingSender() {
        // 文件尚未存檔（沒有路徑）時，沒有東西可以附加——必須在呼叫 sender
        // 之前就明確失敗，不能寄一封看起來附了檔案、實際上沒有的信。
        DocumentEmailRequest request;
        request.to = QStringLiteral("reviewer@example.com");

        bool senderCalled = false;
        EmailSender fakeSender = [&](const EmailComposeRequest&) {
            senderCalled = true;
            return EmailComposeResult{true, {}};
        };

        const EmailComposeResult result = sendDocumentByEmail(request, fakeSender);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!senderCalled);
    }

    void missingFileOnDiskFailsBeforeCallingSender() {
        DocumentEmailRequest request;
        request.documentPath = QStringLiteral("Z:/this/path/does/not/exist.pdf");

        bool senderCalled = false;
        EmailSender fakeSender = [&](const EmailComposeRequest&) {
            senderCalled = true;
            return EmailComposeResult{true, {}};
        };

        const EmailComposeResult result = sendDocumentByEmail(request, fakeSender);
        QVERIFY(!result.ok);
        QVERIFY(!senderCalled);
    }

    void senderFailureIsPropagatedNotSwallowed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString docPath = dir.filePath(QStringLiteral("x.pdf"));
        QFile file(docPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.7 fake");
        file.close();

        DocumentEmailRequest request;
        request.documentPath = docPath;

        EmailSender failingSender = [](const EmailComposeRequest&) {
            return EmailComposeResult{false, QStringLiteral("模擬找不到郵件用戶端")};
        };

        const EmailComposeResult result = sendDocumentByEmail(request, failingSender);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestDocumentEmail)
#include "test_document_email.moc"
