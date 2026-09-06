#include "platform/paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace alioth::platform {
namespace {

QString ensured(QString path) {
    ensureDirectory(path);
    return path;
}

}  // namespace

QString configDirectory() {
    return ensured(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
}

QString cacheDirectory() {
    return ensured(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
}

QString autosaveDirectory() {
    return ensured(cacheDirectory() + QStringLiteral("/autosave"));
}

bool ensureDirectory(const QString& path) {
    if (path.isEmpty()) return false;
    QDir dir;
    return dir.mkpath(path);
}

QString canonicalFileKey(const QString& path) {
    if (path.isEmpty()) return {};
    // canonicalFilePath 會解析符號連結，但檔案不存在時回傳空字串。
    // 歷史紀錄必須容納已經被移走的檔案，因此取不到時退回 absoluteFilePath。
    const QFileInfo info(path);
    QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty()) resolved = QDir::cleanPath(info.absoluteFilePath());
#ifdef _WIN32
    // Windows 檔案系統不分大小寫。不折疊的話，從命令列與從檔案總管開同一份檔案
    // 會產生兩筆紀錄，而使用者看到的是同一個檔名重複出現兩次。
    resolved = resolved.toLower();
#endif
    return resolved;
}

}  // namespace alioth::platform
