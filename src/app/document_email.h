#pragma once

// Email 整份文件（PRD-IO-011）。
//
// 與 form_email.h（PRD-FORM-024）的唯一差別：附件是文件本身在磁碟上的路徑，
// 不是先產生一份匯出檔再寫暫存。文件已經是一個檔案，直接把路徑交給
// platform::composeAndSendEmail 即可，不需要多一次複製。
//
// 寄送機制（Simple MAPI）完全收斂在 platform::composeAndSendEmail /
// defaultEmailSender()，這裡只負責組請求與這一層特有的驗證：
// 文件必須已經存過（有路徑），否則沒有東西可以附加。
//
// 絕對不可在測試裡真的呼叫使用者的郵件用戶端：一律注入 EmailSender，
// 測試只驗證「請求組得對不對」，不驗證「真的寄出去了」（見 form_email.h 同理）。

#include <QString>

#include "platform/email_compose.h"

namespace alioth::app {

struct DocumentEmailRequest {
    QString documentPath;  // 目前開啟文件的磁碟路徑；必須非空且檔案存在
    QString to;
    QString subject;  // 留空則以檔名產生預設主旨
    QString body;
};

// documentPath 為空時視為「文件尚未存檔，沒有東西可以附加」，在呼叫 sender 之前
// 就明確失敗（IL-4）——不能默默地寄一封沒有附件的信讓使用者誤以為附上了。
// 檔案是否存在的檢查交給 composeAndSendEmail，避免兩處各寫一次同樣的邏輯。
[[nodiscard]] platform::EmailComposeResult sendDocumentByEmail(
    const DocumentEmailRequest& request, const platform::EmailSender& sender);

}  // namespace alioth::app
