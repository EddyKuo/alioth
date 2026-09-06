#include "platform/external_process.h"

#include <QFileInfo>
#include <QProcess>

namespace alioth::platform {

namespace {

ExternalProcessLaunchResult launchViaQProcess(const ExternalProcessRequest& request) {
    if (!QFileInfo::exists(request.executablePath)) {
        return ExternalProcessLaunchResult{
            false, QStringLiteral("執行檔不存在：%1").arg(request.executablePath)};
    }

    // startDetached：本程式結束後外部工具不應該被跟著砍掉（例如使用者拿去開的是
    // 一個要長時間跑的比對工具）。不帶任何 runas / elevate 參數——見標頭檔說明。
    qint64 pid = 0;
    const bool started = QProcess::startDetached(request.executablePath, request.arguments,
                                                  request.workingDirectory, &pid);
    if (!started) {
        return ExternalProcessLaunchResult{
            false, QStringLiteral("無法啟動外部程式：%1").arg(request.executablePath)};
    }
    return ExternalProcessLaunchResult{true, {}};
}

}  // namespace

ExternalProcessLauncher defaultExternalProcessLauncher() { return &launchViaQProcess; }

}  // namespace alioth::platform
