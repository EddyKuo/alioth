#include "platform/email_compose.h"

#include <QFileInfo>

#ifdef _WIN32
// 動態載入而不是靜態連結 mapi32.lib：Simple MAPI 由已安裝的郵件用戶端
// （Outlook、Windows Mail…）提供 MAPI32.dll，沒有安裝任何郵件用戶端的機器上
// 這個 DLL 可能根本不存在。靜態連結會讓連結期就依賴一個執行期才知道
// 存不存在的元件；動態載入让「找不到」變成一個可以明確回報的執行期錯誤，
// 而不是編譯或啟動失敗。
#include <windows.h>

#include <mapi.h>
#endif

namespace alioth::platform {

namespace {

#ifdef _WIN32

using MapiSendMailProc = ULONG(WINAPI*)(LHANDLE, ULONG_PTR, lpMapiMessage, FLAGS, ULONG);

EmailComposeResult sendViaSimpleMapi(const EmailComposeRequest& request) {
    HMODULE mapiModule = LoadLibraryW(L"MAPI32.dll");
    if (mapiModule == nullptr) {
        return EmailComposeResult{false,
                                  QStringLiteral("找不到 MAPI32.dll：這台機器可能沒有安裝"
                                                 "支援 Simple MAPI 的郵件用戶端（例如 Outlook）")};
    }

    auto sendMail = reinterpret_cast<MapiSendMailProc>(
        reinterpret_cast<void*>(GetProcAddress(mapiModule, "MAPISendMail")));
    if (sendMail == nullptr) {
        FreeLibrary(mapiModule);
        return EmailComposeResult{false,
                                  QStringLiteral("MAPI32.dll 沒有匯出 MAPISendMail，"
                                                 "郵件用戶端可能安裝不完整")};
    }

    const QByteArray subjectBytes = request.subject.toLocal8Bit();
    const QByteArray bodyBytes = request.body.toLocal8Bit();
    const QByteArray toBytes = request.to.toLocal8Bit();
    const QByteArray attachmentPathBytes = request.attachmentPath.toLocal8Bit();

    MapiRecipDesc recipient{};
    recipient.ulRecipClass = MAPI_TO;
    // const_cast：Simple MAPI 的結構體定義沒有 const 欄位，是這個上世紀 API
    // 的先天限制，不是我們能改的；MAPISendMail 本身不會寫回這塊記憶體。
    recipient.lpszName = const_cast<char*>(toBytes.constData());

    MapiFileDesc fileDesc{};
    const bool hasAttachment = !request.attachmentPath.isEmpty();
    if (hasAttachment) {
        fileDesc.nPosition = static_cast<ULONG>(-1);
        fileDesc.lpszPathName = const_cast<char*>(attachmentPathBytes.constData());
    }

    MapiMessage message{};
    message.lpszSubject = const_cast<char*>(subjectBytes.constData());
    message.lpszNoteText = const_cast<char*>(bodyBytes.constData());
    if (!request.to.isEmpty()) {
        message.nRecipCount = 1;
        message.lpRecips = &recipient;
    }
    if (hasAttachment) {
        message.nFileCount = 1;
        message.lpFiles = &fileDesc;
    }

    // MAPI_DIALOG：讓郵件用戶端跳出撰寫視窗給使用者確認再送出，而不是
    // 靜默背景發信。PRD 的用意是「把資料交給郵件用戶端」，使用者仍應該
    // 在寄出前看得到內容——這與「開啟網址前需確認」是同一個立場。
    const ULONG status = sendMail(0, 0, &message, MAPI_DIALOG | MAPI_LOGON_UI, 0);
    FreeLibrary(mapiModule);

    if (status != SUCCESS_SUCCESS && status != MAPI_USER_ABORT) {
        return EmailComposeResult{false,
                                  QStringLiteral("MAPISendMail 回報錯誤碼 %1").arg(status)};
    }
    return EmailComposeResult{true, {}};
}

#else

EmailComposeResult sendViaSimpleMapi(const EmailComposeRequest&) {
    // 平台決策（CLAUDE.md）：先做純 Windows。macOS／Linux 的寄送機制未實作，
    // 明確回報 BLOCKED 而不是假裝寄出去了。
    return EmailComposeResult{false, QStringLiteral("本平台尚未實作 Email 表單資料寄送")};
}

#endif

}  // namespace

EmailSender defaultEmailSender() { return &sendViaSimpleMapi; }

EmailComposeResult composeAndSendEmail(const EmailComposeRequest& request,
                                       const EmailSender& sender) {
    if (!request.attachmentPath.isEmpty() && !QFileInfo::exists(request.attachmentPath)) {
        // 在呼叫 sender 之前就擋下來：把不存在的路徑交給郵件用戶端，
        // 多數用戶端會直接略過附件而不報錯，使用者會以為附件已經附上。
        return EmailComposeResult{false,
                                  QStringLiteral("附件檔案不存在：%1").arg(request.attachmentPath)};
    }
    if (!sender) {
        return EmailComposeResult{false, QStringLiteral("沒有可用的寄送函式")};
    }
    return sender(request);
}

}  // namespace alioth::platform
