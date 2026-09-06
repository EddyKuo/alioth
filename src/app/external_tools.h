#pragma once

// 第三方程式工具列（PRD-UI-016，R3／C）。
//
// 這是全案安全敏感度最高的功能之一：使用者能設定「按一個按鈕就啟動任意外部程式」。
// 交付要求列出的四條硬性限制，能在型別層級擋住的就不留給使用習慣去保證：
//
//   1. 不繼承本程式的權限提升——啟動一律走一般權限的 CreateProcess（實際執行者
//      platform::defaultExternalProcessLauncher 用的是 QProcess::startDetached，
//      沒有任何路徑呼叫 ShellExecute 的 runas 動詞，子行程的完整性層級不會
//      高於本程式）。這件事發生在 platform 層，本檔案只負責組出「要執行什麼」。
//   2. 參數不可由 PDF 內容決定——ExternalTool::argumentsTemplate 只認得一個
//      佔位字串 {file}，展開時只會被换成「目前開啟文件的路徑」，
//      而那個路徑是應用層從已開啟的文件把手取得的檔案系統路徑，不是從
//      PDF 物件、標註、表單值、Metadata 等來自檔案內容的不可信輸入讀出來的。
//      任何其他 {...} 樣式一律在驗證階段拒絕（見 DisallowedPlaceholder），
//      這樣未來就算有人想「方便一點」多接一個欄位，型別系統會先擋下來。
//   3. 啟動前顯示完整命令列供確認——本檔案只負責組出可顯示的字串
//      （formatCommandLineForConfirmation），真正彈出確認框是呈現層的事
//      （見 ui/external_tool_dialogs.h 的 ExternalToolConfirmDialog）。
//   4. 絕不從文件中的 Launch Action 觸發——engine 層已經在來源端完全忽略
//      Launch Action（engine/pdfium_engine.cpp 的 FPDFACTION_LAUNCH 分支），
//      這裡的 API 也沒有任何入口接受「文件內建議的命令」；
//      呼叫端唯一能取得 ExternalToolLaunchRequest 的方式是使用者在
//      設定畫面手動建立、儲存在本機設定裡的 ExternalTool。

#include <QString>
#include <QStringList>

#include "platform/external_process.h"

namespace alioth::app {

struct ExternalTool {
    QString name;               // 顯示在工具列／選單上的名稱
    QString executablePath;     // 絕對路徑；必須實際存在才允許啟動
    QString argumentsTemplate;  // 以空白分隔的參數樣板，僅允許 {file} 佔位字元
    QString workingDirectory;   // 留空則使用執行檔所在目錄
};

enum class ExternalToolValidationError {
    None,
    NameEmpty,
    ExecutablePathEmpty,
    ExecutablePathNotAbsolute,
    ExecutableNotFound,
    DisallowedPlaceholder,  // 出現 {file} 以外的 {...} 佔位字元
};

[[nodiscard]] QString describeValidationError(ExternalToolValidationError error);

// 只檢查「設定本身合不合法」；不檢查使用者是否有權限執行它——那是作業系統
// 的事，應用層不代為判斷，也不應該讓「代為判斷」變成繞過作業系統權限模型的路。
[[nodiscard]] ExternalToolValidationError validateExternalTool(const ExternalTool& tool);

struct ExternalToolLaunchRequest {
    QString executablePath;
    QStringList arguments;
    QString workingDirectory;
};

// 用目前文件路徑展開 {file} 佔位字元。currentDocumentPath 可以是空字串
// （沒有開啟文件時使用者仍可能想啟動一個跟文件無關的工具），此時 {file}
// 會展開成空字串而不是報錯；是否要擋下「沒有文件時啟動」，由呼叫端決定。
[[nodiscard]] ExternalToolLaunchRequest buildLaunchRequest(const ExternalTool& tool,
                                                           const QString& currentDocumentPath);

// 供確認對話框顯示的完整命令列，例如："C:\Tools\foo.exe" "C:\doc.pdf"
// 每個參數都以雙引號包起來，讓含空白的路徑不會被誤讀成兩個參數。
// 這只是「給人看」的字串——真正傳給作業系統的仍是 QStringList
// （見 ExternalToolLaunchRequest::arguments），不會被 shell 重新解析，
// 因此這裡的引號不是注入風險的來源，純粹是給使用者確認用。
[[nodiscard]] QString formatCommandLineForConfirmation(const ExternalToolLaunchRequest& request);

// 轉成平台層的請求型別。平台層不得相依應用層（見 platform/external_process.h
// 的說明），所以轉換動作放在應用層這一側。
[[nodiscard]] platform::ExternalProcessRequest toExternalProcessRequest(
    const ExternalToolLaunchRequest& request);

}  // namespace alioth::app
