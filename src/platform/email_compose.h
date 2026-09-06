#pragma once

// Email 表單資料（PRD-FORM-024）。
//
// 「寄送」本身是平台層的事：三個作業系統上「把一封含附件的郵件交給使用者的
// 預設郵件用戶端」完全是三套不同的機制（Windows 是 Simple MAPI、macOS 是
// NSSharingService、Linux 沒有統一標準，得看 xdg-email 或個別客戶端）。
// 這裡只做 Windows 這一份，符合「先做純 Windows」的決策——但介面刻意設計成
// 平台無關，未來加其他平台只需要換掉 defaultEmailSender() 的實作。
//
// 產生 FDF/XFDF 內容不是本檔案的事，那是 engine/forms/form_data.h 的純函數；
// 這裡只負責「把已經產生好的匯出檔交給郵件用戶端」。
//
// 絕對不可在自動化測試裡真的呼叫使用者的郵件用戶端——那會彈出真實的郵件
// 視窗甚至真的寄出郵件。因此寄送動作被抽成可注入的 EmailSender，
// 測試一律注入假的 sender 驗證「請求組得對不對」，不驗證「真的寄出去了」。

#include <functional>

#include <QString>

namespace alioth::platform {

struct EmailComposeRequest {
    QString to;              // 收件人；可留空，讓使用者自己在郵件用戶端填
    QString subject;
    QString body;
    QString attachmentPath;  // 已存在的檔案路徑（FDF/XFDF 匯出檔）；留空代表無附件
};

struct EmailComposeResult {
    bool ok{false};
    QString error;  // ok == false 時必有原因（IL-4），繁體中文可直接顯示
};

using EmailSender = std::function<EmailComposeResult(const EmailComposeRequest&)>;

// Windows 上以 Simple MAPI（動態載入 MAPI32.dll，不靜態連結——理由見 .cpp）
// 呼叫使用者的預設郵件用戶端。找不到已安裝的郵件用戶端時回傳明確的失敗，
// 不僅僅是回傳 false：使用者需要知道是「沒裝郵件軟體」還是「附件檔案不存在」。
[[nodiscard]] EmailSender defaultEmailSender();

// 組出請求並呼叫 sender。附件路徑若非空但檔案不存在，在呼叫 sender 之前就
// 明確失敗——把一個不存在的附件路徑交給郵件用戶端，多數情況下用戶端會
// 直接忽略附件而不報錯，使用者會以為附件已經附上。
[[nodiscard]] EmailComposeResult composeAndSendEmail(const EmailComposeRequest& request,
                                                     const EmailSender& sender);

}  // namespace alioth::platform
