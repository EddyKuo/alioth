#pragma once

// Email 表單資料（PRD-FORM-024）。
//
// 這一層只做「膠水」：把 engine::forms::exportFormData 產出的 FDF/XFDF 位元組
// 寫成一個暫存檔，再交給 platform::composeAndSendEmail 讓使用者的郵件用戶端
// 接手。真正的寄送機制（Simple MAPI）收斂在平台層，這裡完全不碰作業系統 API。
//
// 絕對不可在測試裡真的寄信：sendFormDataByEmail 一律要求呼叫端提供
// EmailSender，測試注入假的 sender 驗證「請求組得對不對」，正式執行才用
// platform::defaultEmailSender()。

#include <QString>

#include "platform/email_compose.h"

namespace alioth::app {

struct FormEmailRequest {
    QString exportedText;        // engine::forms::exportFormData 的輸出（FDF 或 XFDF）
    QString attachmentFileName;  // 例如 "form-data.xfdf"；決定副檔名讓收件人的
                                 // 郵件用戶端與 Acrobat 認得出格式
    QString to;
    QString subject;
    QString body;
};

// 把匯出文字寫進暫存目錄（platform::cacheDirectory()/email_export/），
// 再交給 sender。寫檔失敗（例如磁碟空間不足）與寄送失敗都回傳明確原因，
// 不得只回傳 false（IL-4）。
[[nodiscard]] platform::EmailComposeResult sendFormDataByEmail(
    const FormEmailRequest& request, const platform::EmailSender& sender);

}  // namespace alioth::app
