#include "app/external_tools.h"

#include <QDir>
#include <QFileInfo>

namespace alioth::app {

namespace {

// 把 argumentsTemplate 切成參數清單。支援用雙引號包住含空白的單一參數
// （例如 "--config" "C:\\some path\\a.ini"），語法故意做得極簡——這是設定畫面
// 裡使用者自己打的樣板，不需要支援完整的 shell 語法。
QStringList tokenize(const QString& templateText) {
    QStringList tokens;
    QString current;
    bool inQuotes = false;
    bool hasToken = false;

    for (const QChar ch : templateText) {
        if (ch == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            hasToken = true;
            continue;
        }
        if (ch.isSpace() && !inQuotes) {
            if (hasToken) {
                tokens.push_back(current);
                current.clear();
                hasToken = false;
            }
            continue;
        }
        current.append(ch);
        hasToken = true;
    }
    if (hasToken) tokens.push_back(current);
    return tokens;
}

// 掃描一個字串裡所有 {...} 樣式的佔位字元，回傳裡面的內容（不含大括號）。
// 用來源自: "{file}.bak" -> ["file"]；"{file}{other}" -> ["file", "other"]。
QStringList extractPlaceholders(const QString& text) {
    QStringList result;
    int i = 0;
    while (i < text.size()) {
        if (text[i] == QLatin1Char('{')) {
            const int close = text.indexOf(QLatin1Char('}'), i + 1);
            if (close < 0) break;  // 沒有配對的右括號，交給呼叫端當作一般文字
            result.push_back(text.mid(i + 1, close - i - 1));
            i = close + 1;
        } else {
            ++i;
        }
    }
    return result;
}

}  // namespace

QString describeValidationError(ExternalToolValidationError error) {
    switch (error) {
        case ExternalToolValidationError::None:
            return QStringLiteral("");
        case ExternalToolValidationError::NameEmpty:
            return QStringLiteral("工具名稱不可留空");
        case ExternalToolValidationError::ExecutablePathEmpty:
            return QStringLiteral("必須指定執行檔路徑");
        case ExternalToolValidationError::ExecutablePathNotAbsolute:
            return QStringLiteral("執行檔路徑必須是絕對路徑，不可依賴目前工作目錄");
        case ExternalToolValidationError::ExecutableNotFound:
            return QStringLiteral("找不到指定的執行檔");
        case ExternalToolValidationError::DisallowedPlaceholder:
            return QStringLiteral("參數樣板只允許 {file} 這個佔位字元");
    }
    return QStringLiteral("未知的驗證錯誤");
}

ExternalToolValidationError validateExternalTool(const ExternalTool& tool) {
    if (tool.name.trimmed().isEmpty()) return ExternalToolValidationError::NameEmpty;
    if (tool.executablePath.isEmpty()) return ExternalToolValidationError::ExecutablePathEmpty;

    const QFileInfo info(tool.executablePath);
    if (!info.isAbsolute()) return ExternalToolValidationError::ExecutablePathNotAbsolute;
    if (!info.exists() || !info.isFile()) return ExternalToolValidationError::ExecutableNotFound;

    for (const QString& placeholder : extractPlaceholders(tool.argumentsTemplate)) {
        if (placeholder != QStringLiteral("file")) {
            return ExternalToolValidationError::DisallowedPlaceholder;
        }
    }
    return ExternalToolValidationError::None;
}

ExternalToolLaunchRequest buildLaunchRequest(const ExternalTool& tool,
                                             const QString& currentDocumentPath) {
    ExternalToolLaunchRequest request;
    request.executablePath = tool.executablePath;

    for (QString token : tokenize(tool.argumentsTemplate)) {
        // 只替換 {file}，其餘 {...} 樣式在 validateExternalTool 就已經被拒絕，
        // 走到這裡代表呼叫端要嘛驗證過、要嘛自己承擔風險——這裡不重複拒絕，
        // 因為 buildLaunchRequest 的職責是「展開」，不是「守門」。
        token.replace(QStringLiteral("{file}"), currentDocumentPath);
        request.arguments.push_back(token);
    }

    request.workingDirectory = tool.workingDirectory.isEmpty()
                                   ? QFileInfo(tool.executablePath).absolutePath()
                                   : tool.workingDirectory;
    return request;
}

QString formatCommandLineForConfirmation(const ExternalToolLaunchRequest& request) {
    QString line = QStringLiteral("\"%1\"").arg(request.executablePath);
    for (const QString& argument : request.arguments) {
        line += QStringLiteral(" \"%1\"").arg(argument);
    }
    return line;
}

platform::ExternalProcessRequest toExternalProcessRequest(
    const ExternalToolLaunchRequest& request) {
    platform::ExternalProcessRequest processRequest;
    processRequest.executablePath = request.executablePath;
    processRequest.arguments = request.arguments;
    processRequest.workingDirectory = request.workingDirectory;
    return processRequest;
}

}  // namespace alioth::app
