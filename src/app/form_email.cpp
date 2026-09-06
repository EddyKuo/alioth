#include "app/form_email.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "platform/paths.h"

namespace alioth::app {

namespace {

[[nodiscard]] platform::EmailComposeResult writeAttachment(const FormEmailRequest& request,
                                                            QString& outPath) {
    QString directory = platform::cacheDirectory() + QStringLiteral("/email_export");
    if (!platform::ensureDirectory(directory)) {
        return platform::EmailComposeResult{
            false, QStringLiteral("無法建立暫存目錄：%1").arg(directory)};
    }

    QString fileName = request.attachmentFileName;
    if (fileName.isEmpty()) fileName = QStringLiteral("form-data.xfdf");

    outPath = directory + QStringLiteral("/") + fileName;
    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return platform::EmailComposeResult{false,
                                            QStringLiteral("無法寫入暫存附件：%1").arg(outPath)};
    }
    const QByteArray bytes = request.exportedText.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        return platform::EmailComposeResult{false,
                                            QStringLiteral("寫入暫存附件不完整：%1").arg(outPath)};
    }
    file.close();
    return platform::EmailComposeResult{true, {}};
}

}  // namespace

platform::EmailComposeResult sendFormDataByEmail(const FormEmailRequest& request,
                                                 const platform::EmailSender& sender) {
    QString attachmentPath;
    const platform::EmailComposeResult writeResult = writeAttachment(request, attachmentPath);
    if (!writeResult.ok) return writeResult;

    platform::EmailComposeRequest composeRequest;
    composeRequest.to = request.to;
    composeRequest.subject = request.subject;
    composeRequest.body = request.body;
    composeRequest.attachmentPath = attachmentPath;
    return platform::composeAndSendEmail(composeRequest, sender);
}

}  // namespace alioth::app
