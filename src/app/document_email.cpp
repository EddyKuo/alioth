#include "app/document_email.h"

#include <QFileInfo>

namespace alioth::app {

platform::EmailComposeResult sendDocumentByEmail(const DocumentEmailRequest& request,
                                                 const platform::EmailSender& sender) {
    if (request.documentPath.isEmpty()) {
        return platform::EmailComposeResult{
            false, QStringLiteral("文件尚未存檔，沒有可附加的檔案")};
    }

    const QFileInfo info(request.documentPath);

    platform::EmailComposeRequest composeRequest;
    composeRequest.to = request.to;
    composeRequest.subject =
        request.subject.isEmpty() ? QStringLiteral("文件：%1").arg(info.fileName())
                                  : request.subject;
    composeRequest.body = request.body;
    composeRequest.attachmentPath = request.documentPath;
    return platform::composeAndSendEmail(composeRequest, sender);
}

}  // namespace alioth::app
