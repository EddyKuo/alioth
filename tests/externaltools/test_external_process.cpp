// 第三方程式的實際啟動（PRD-UI-016）。
//
// 安全限制 #1「不繼承本程式的權限提升」在這一層的可測試表現是：實作只呼叫
// QProcess::startDetached，沒有任何條件分支會換成 ShellExecuteEx 的
// runas 動詞——這件事無法用執行期斷言直接驗證（沒有 API 能問「這個行程
// 是不是用一般權限啟動的」而不牽涉作業系統層級的測試基礎設施），所以
// 這裡改為驗證行為上可觀察的部分：執行檔不存在時明確失敗、執行檔存在時
// 真的啟動成功。程式碼審查與 platform/external_process.h 的說明是
// 「不呼叫 runas」這件事的實際防線。

#include <QtTest>

#include <QStandardPaths>

#include "platform/external_process.h"

using namespace alioth::platform;

class TestExternalProcess : public QObject {
    Q_OBJECT

private slots:
    void missingExecutableFailsWithoutStartingAnything() {
        ExternalProcessRequest request;
        request.executablePath = QStringLiteral("C:/this/path/does/not/exist/tool.exe");

        const ExternalProcessLauncher launcher = defaultExternalProcessLauncher();
        const ExternalProcessLaunchResult result = launcher(request);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
    }

    void existingExecutableStartsSuccessfully() {
        // 用系統一定找得到、啟動後會立刻自行結束的執行檔，驗證「傳給
        // QProcess::startDetached 的三個欄位真的被使用」，而不真的去驗證
        // 某個第三方工具的行為。
        const QString cmdPath = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
        if (cmdPath.isEmpty()) {
            QSKIP("找不到 cmd.exe，略過（非 Windows 或 PATH 異常環境）");
        }

        ExternalProcessRequest request;
        request.executablePath = cmdPath;
        request.arguments = {QStringLiteral("/c"), QStringLiteral("exit")};

        const ExternalProcessLauncher launcher = defaultExternalProcessLauncher();
        const ExternalProcessLaunchResult result = launcher(request);
        QVERIFY2(result.ok, qPrintable(result.error));
    }
};

QTEST_GUILESS_MAIN(TestExternalProcess)
#include "test_external_process.moc"
