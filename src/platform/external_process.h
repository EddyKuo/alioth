#pragma once

// 第三方程式的實際啟動（PRD-UI-016）。呼叫端（應用層的 app::ExternalTool 相關
// 邏輯）已經把樣板展開、驗證過，這裡只做「交給作業系統」這一件事——不解析
// 樣板、不碰 PDF、不做任何與文件內容有關的決策。
//
// 這個結構體刻意不是 app::ExternalToolLaunchRequest：平台層是分層的最底層
// （見 docs/SDD.md §2），不得反過來相依應用層，否則往下移植到其他平台時，
// 平台層會被拖著一起帶走它原本不需要知道的應用邏輯。呼叫端自行把
// app::ExternalToolLaunchRequest 轉成這裡的 ExternalProcessRequest。
//
// 安全上唯一要守住的一條：啟動方式不得提升權限。QProcess::startDetached
// 底層在 Windows 上是不帶 runas 動詞的 CreateProcess，子行程的權杖與完整性
// 層級直接繼承自本程式，不會因為使用者按了一下工具列按鈕就變成系統管理員——
// 這與 ShellExecuteEx 搭配 "runas" 動詞是兩回事，本檔案任何實作都不可以
// 換成後者。
//
// 這裡沒有 #ifdef _WIN32：QProcess::startDetached 本身跨平台，真正的作業系統
// 差異（例如要不要用 xdg-open 之類）留給未來真的要做 macOS/Linux 時再處理，
// 屆時仍然收斂在這一個檔案，不擴散到呼叫端。

#include <functional>

#include <QString>
#include <QStringList>

namespace alioth::platform {

struct ExternalProcessRequest {
    QString executablePath;
    QStringList arguments;
    QString workingDirectory;
};

struct ExternalProcessLaunchResult {
    bool ok{false};
    QString error;  // ok == false 時必有原因（IL-4）
};

using ExternalProcessLauncher =
    std::function<ExternalProcessLaunchResult(const ExternalProcessRequest&)>;

// 真正呼叫 QProcess::startDetached。啟動前只做「執行檔是否存在」這類與
// 命令本身無關的最後一道防線；命令合不合法（例如佔位字元）已經在
// app::validateExternalTool 做過，這裡不重複規則，只負責執行。
[[nodiscard]] ExternalProcessLauncher defaultExternalProcessLauncher();

}  // namespace alioth::platform
