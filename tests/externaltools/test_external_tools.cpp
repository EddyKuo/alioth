// 第三方程式工具列（PRD-UI-016）的驗證與請求組裝邏輯。
//
// 這是全案安全敏感度最高的功能之一，測試重點放在四條硬性限制裡「能在
// 型別層級驗證的那兩條」：
//   - 參數不可由 PDF 內容決定：任何非 {file} 的 {...} 佔位字元一律拒絕
//   - 啟動前顯示完整命令列供確認：驗證 formatCommandLineForConfirmation
//     真的把每個參數都包進去，不會因為某個參數含空白而讓命令列可讀性
//     產生歧義（例如把兩個參數誤看成一個）
//
// 「不繼承本程式的權限提升」與「絕不從 Launch Action 觸發」這兩條無法
// 在這一層用單元測試直接斷言（它們是架構層級的保證：見
// platform/external_process.h 只用 QProcess::startDetached，以及
// engine/pdfium_engine.cpp 完全忽略 Launch Action），這裡改用注釋標記
// 對應位置，並在 test_external_process.cpp 驗證啟動路徑本身不吃任何
// elevate 參數。

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include "app/external_tools.h"

using namespace alioth::app;

class TestExternalTools : public QObject {
    Q_OBJECT

private slots:
    void rejectsEmptyName() {
        ExternalTool tool;
        tool.name = QStringLiteral("   ");
        tool.executablePath = QCoreApplication::applicationFilePath();
        QCOMPARE(validateExternalTool(tool), ExternalToolValidationError::NameEmpty);
    }

    void rejectsEmptyExecutablePath() {
        ExternalTool tool;
        tool.name = QStringLiteral("工具");
        QCOMPARE(validateExternalTool(tool), ExternalToolValidationError::ExecutablePathEmpty);
    }

    void rejectsRelativeExecutablePath() {
        ExternalTool tool;
        tool.name = QStringLiteral("工具");
        tool.executablePath = QStringLiteral("tool.exe");
        QCOMPARE(validateExternalTool(tool),
                 ExternalToolValidationError::ExecutablePathNotAbsolute);
    }

    void rejectsMissingExecutable() {
        ExternalTool tool;
        tool.name = QStringLiteral("工具");
        tool.executablePath = QStringLiteral("C:/this/path/does/not/exist/tool.exe");
        QCOMPARE(validateExternalTool(tool), ExternalToolValidationError::ExecutableNotFound);
    }

    void rejectsDisallowedPlaceholder() {
        // 這是安全限制 #2 的核心斷言：{file} 以外的任何 {...} 一律拒絕，
        // 未來就算有人想「方便一點」多接一個來自文件內容的欄位
        // （例如 {author} 或 {title}），這裡會先擋下來。
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ExternalTool tool;
        tool.name = QStringLiteral("工具");
        tool.executablePath = QCoreApplication::applicationFilePath();
        tool.argumentsTemplate = QStringLiteral("--title \"{title}\"");
        QCOMPARE(validateExternalTool(tool), ExternalToolValidationError::DisallowedPlaceholder);
    }

    void acceptsValidToolWithFilePlaceholder() {
        ExternalTool tool;
        tool.name = QStringLiteral("工具");
        tool.executablePath = QCoreApplication::applicationFilePath();
        tool.argumentsTemplate = QStringLiteral("--open {file}");
        QCOMPARE(validateExternalTool(tool), ExternalToolValidationError::None);
    }

    void buildLaunchRequestExpandsFilePlaceholderOnly() {
        ExternalTool tool;
        tool.name = QStringLiteral("比對工具");
        tool.executablePath = QStringLiteral("C:/Tools/diff.exe");
        tool.argumentsTemplate = QStringLiteral("--readonly {file}");

        const ExternalToolLaunchRequest request =
            buildLaunchRequest(tool, QStringLiteral("C:/Docs/report.pdf"));

        QCOMPARE(request.executablePath, tool.executablePath);
        QCOMPARE(request.arguments.size(), 2);
        QCOMPARE(request.arguments.at(0), QStringLiteral("--readonly"));
        QCOMPARE(request.arguments.at(1), QStringLiteral("C:/Docs/report.pdf"));
    }

    void buildLaunchRequestSupportsQuotedArgumentsWithSpaces() {
        ExternalTool tool;
        tool.name = QStringLiteral("工具");
        tool.executablePath = QStringLiteral("C:/Tools/tool.exe");
        tool.argumentsTemplate = QStringLiteral("--file \"{file}\" --mode \"read only\"");

        const ExternalToolLaunchRequest request =
            buildLaunchRequest(tool, QStringLiteral("C:/My Docs/a b.pdf"));

        QCOMPARE(request.arguments.size(), 4);
        QCOMPARE(request.arguments.at(0), QStringLiteral("--file"));
        // {file} 在引號內展開，路徑本身含空白，但因為 arguments 是 QStringList
        // 逐一傳給 QProcess，不會被 shell 重新切割成兩個參數。
        QCOMPARE(request.arguments.at(1), QStringLiteral("C:/My Docs/a b.pdf"));
        QCOMPARE(request.arguments.at(2), QStringLiteral("--mode"));
        QCOMPARE(request.arguments.at(3), QStringLiteral("read only"));
    }

    void buildLaunchRequestAllowsEmptyDocumentPath() {
        // 沒有開啟文件時仍可能想啟動一個跟文件無關的工具；{file} 展開成空字串，
        // 不應該讓 buildLaunchRequest 本身失敗——是否要擋下這種啟動由呼叫端決定。
        ExternalTool tool;
        tool.executablePath = QStringLiteral("C:/Tools/tool.exe");
        tool.argumentsTemplate = QStringLiteral("{file}");
        const ExternalToolLaunchRequest request = buildLaunchRequest(tool, QString());
        QCOMPARE(request.arguments.size(), 1);
        QVERIFY(request.arguments.at(0).isEmpty());
    }

    void workingDirectoryDefaultsToExecutableDirectory() {
        ExternalTool tool;
        tool.executablePath = QStringLiteral("C:/Tools/sub/tool.exe");
        const ExternalToolLaunchRequest request = buildLaunchRequest(tool, QString());
        QCOMPARE(request.workingDirectory, QStringLiteral("C:/Tools/sub"));
    }

    void explicitWorkingDirectoryIsPreserved() {
        ExternalTool tool;
        tool.executablePath = QStringLiteral("C:/Tools/tool.exe");
        tool.workingDirectory = QStringLiteral("D:/Work");
        const ExternalToolLaunchRequest request = buildLaunchRequest(tool, QString());
        QCOMPARE(request.workingDirectory, QStringLiteral("D:/Work"));
    }

    void formatCommandLineQuotesEveryArgument() {
        // 安全限制 #3：啟動前必須能顯示「完整」命令列供使用者確認，
        // 每個參數都要看得清楚邊界在哪裡，含空白的參數不能讓人誤讀成兩段。
        ExternalToolLaunchRequest request;
        request.executablePath = QStringLiteral("C:/Tools/diff.exe");
        request.arguments = {QStringLiteral("--readonly"), QStringLiteral("C:/My Docs/a.pdf")};

        const QString formatted = formatCommandLineForConfirmation(request);
        QCOMPARE(formatted,
                 QStringLiteral("\"C:/Tools/diff.exe\" \"--readonly\" \"C:/My Docs/a.pdf\""));
    }

    void conversionToProcessRequestPreservesAllFields() {
        ExternalToolLaunchRequest request;
        request.executablePath = QStringLiteral("C:/Tools/tool.exe");
        request.arguments = {QStringLiteral("a"), QStringLiteral("b")};
        request.workingDirectory = QStringLiteral("C:/Tools");

        const auto processRequest = toExternalProcessRequest(request);
        QCOMPARE(processRequest.executablePath, request.executablePath);
        QCOMPARE(processRequest.arguments, request.arguments);
        QCOMPARE(processRequest.workingDirectory, request.workingDirectory);
    }
};

QTEST_GUILESS_MAIN(TestExternalTools)
#include "test_external_tools.moc"
