#include "engine/save/autosave.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include "platform/atomic_file.h"
#include "platform/paths.h"

namespace alioth::engine::save {
namespace {

constexpr auto kPdfSuffix = ".autosave.pdf";
constexpr auto kSidecarSuffix = ".autosave.json";

QString resolveDirectory(const std::string& directory) {
    if (!directory.empty()) {
        const QString path = QString::fromStdString(directory);
        platform::ensureDirectory(path);
        return path;
    }
    return platform::autosaveDirectory();
}

// 用原檔絕對路徑的雜湊當檔名，而不是原檔名。原因有二：
// 不同目錄的同名檔不會互相覆蓋，且自動儲存目錄裡不會洩漏使用者的檔名與路徑
// （PRD §8.2 的隱私立場：診斷資料不含文件內容與路徑）。
QString keyFor(const QString& originalPath) {
    const QString canonical = QDir::cleanPath(QFileInfo(originalPath).absoluteFilePath());
    const QByteArray digest =
        QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex().left(24));
}

bool writeSidecar(const QString& sidecarPath, const AutosaveEntry& entry, QString* error) {
    QJsonObject json;
    json[QStringLiteral("originalPath")] = QString::fromStdString(entry.originalPath);
    json[QStringLiteral("autosavePath")] = QString::fromStdString(entry.autosavePath);
    json[QStringLiteral("savedAtEpochMs")] = static_cast<qint64>(entry.savedAtEpochMs);
    json[QStringLiteral("originalModifiedEpochMs")] =
        static_cast<qint64>(entry.originalModifiedEpochMs);
    json[QStringLiteral("originalSizeBytes")] = static_cast<qint64>(entry.originalSizeBytes);

    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);

    // 側錄檔本身也走原子寫入：復原流程完全信任它，讀到半截 JSON 等於復原功能整個失效。
    platform::AtomicFileWriter writer(sidecarPath);
    if (!writer.begin() || !writer.write(bytes.constData(), static_cast<std::size_t>(bytes.size())) ||
        !writer.commit()) {
        if (error) *error = writer.lastError();
        return false;
    }
    return true;
}

bool readSidecar(const QString& sidecarPath, AutosaveEntry* entry) {
    QFile file(sidecarPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return false;
    const QJsonObject json = doc.object();

    entry->sidecarPath = sidecarPath.toStdString();
    entry->originalPath = json.value(QStringLiteral("originalPath")).toString().toStdString();
    entry->autosavePath = json.value(QStringLiteral("autosavePath")).toString().toStdString();
    entry->savedAtEpochMs = json.value(QStringLiteral("savedAtEpochMs")).toVariant().toLongLong();
    entry->originalModifiedEpochMs =
        json.value(QStringLiteral("originalModifiedEpochMs")).toVariant().toLongLong();
    entry->originalSizeBytes = static_cast<std::uint64_t>(
        json.value(QStringLiteral("originalSizeBytes")).toVariant().toLongLong());
    return entry->valid();
}

AutosaveResult failed(SaveStatus status, const QString& message) {
    AutosaveResult result;
    result.status = status;
    result.message = message.toStdString();
    return result;
}

}  // namespace

AutosaveManager::AutosaveManager(std::string directory)
    : directory_(resolveDirectory(directory).toStdString()) {}

void AutosaveManager::setInterval(std::chrono::seconds interval) {
    // 零或負值會讓自動儲存變成忙碌迴圈，直接夾到 1 秒。
    interval_ = interval.count() > 0 ? interval : std::chrono::seconds{1};
}

void AutosaveManager::markDirty(AutosaveClock::time_point now) {
    if (!dirty_) {
        // 計時從「第一筆未存檔變更」起算，而不是從上次自動儲存起算：
        // 使用者閒置兩小時後打的第一個字，不該立刻觸發一次寫檔。
        lastSave_ = now;
        dirty_ = true;
    }
}

void AutosaveManager::markClean(AutosaveClock::time_point now) {
    dirty_ = false;
    lastSave_ = now;
}

bool AutosaveManager::isDue(AutosaveClock::time_point now) const {
    if (!dirty_) return false;
    return now - lastSave_ >= interval_;
}

AutosaveResult AutosaveManager::autosave(DocumentHandle document, const std::string& originalPath,
                                         AutosaveClock::time_point now) {
    if (!document) {
        return failed(SaveStatus::InvalidDocument, QStringLiteral("文件把手為空"));
    }
    const QString original = QString::fromStdString(originalPath);
    const QFileInfo originalInfo(original);
    if (!originalInfo.isFile() || !originalInfo.isReadable()) {
        return failed(SaveStatus::SourceUnreadable, QStringLiteral("原始檔案無法讀取：") + original);
    }

    const std::string targetPath = autosavePathFor(originalPath, directory_);
    const QString target = QString::fromStdString(targetPath);

    // 最後一道防線。上面的路徑組法已經保證目標在自動儲存目錄下，但這條規則的代價是
    // 使用者的原檔，值得再檢查一次而不是依賴上游正確。
    if (QDir::cleanPath(QFileInfo(target).absoluteFilePath()) ==
        QDir::cleanPath(originalInfo.absoluteFilePath())) {
        return failed(SaveStatus::RefusedOverwriteOriginal,
                      QStringLiteral("自動儲存目標與原檔相同"));
    }

    const SaveResult saved =
        IncrementalSaver::saveIncremental(document, originalPath, targetPath);
    if (!saved.ok()) {
        AutosaveResult result;
        result.status = saved.status;
        result.metrics = saved.metrics;
        result.message = saved.message;
        return result;
    }

    AutosaveEntry entry;
    entry.originalPath = QDir::cleanPath(originalInfo.absoluteFilePath()).toStdString();
    entry.autosavePath = targetPath;
    entry.sidecarPath =
        (QString::fromStdString(directory_) + QStringLiteral("/") +
         keyFor(original) + QLatin1String(kSidecarSuffix))
            .toStdString();
    entry.savedAtEpochMs = QDateTime::currentMSecsSinceEpoch();
    entry.originalModifiedEpochMs = originalInfo.lastModified().toMSecsSinceEpoch();
    entry.originalSizeBytes = static_cast<std::uint64_t>(originalInfo.size());

    QString error;
    if (!writeSidecar(QString::fromStdString(entry.sidecarPath), entry, &error)) {
        // PDF 已經寫成功但側錄失敗：留著孤兒 PDF 只會讓下次掃描看不到它，
        // 不如刪掉，讓狀態保持一致（IL-4，不留半成品）。
        QFile::remove(target);
        return failed(SaveStatus::CommitFailed, QStringLiteral("側錄中繼資料寫入失敗：") + error);
    }

    markClean(now);

    AutosaveResult result;
    result.entry = entry;
    result.metrics = saved.metrics;
    return result;
}

bool AutosaveManager::clearSession(const std::string& originalPath) {
    const QString key = keyFor(QString::fromStdString(originalPath));
    const QString base = QString::fromStdString(directory_) + QStringLiteral("/") + key;
    bool removed = false;
    // 側錄檔先刪：它才是「有東西可復原」的判準，PDF 殘留只是佔空間。
    if (QFile::exists(base + QLatin1String(kSidecarSuffix))) {
        removed = QFile::remove(base + QLatin1String(kSidecarSuffix)) || removed;
    }
    if (QFile::exists(base + QLatin1String(kPdfSuffix))) {
        removed = QFile::remove(base + QLatin1String(kPdfSuffix)) || removed;
    }
    return removed;
}

std::vector<AutosaveEntry> AutosaveManager::findRecoverable(const std::string& directory) {
    std::vector<AutosaveEntry> entries;
    const QString dirPath = resolveDirectory(directory);
    QDir dir(dirPath);
    const QStringList names =
        dir.entryList({QStringLiteral("*%1").arg(QLatin1String(kSidecarSuffix))}, QDir::Files);
    entries.reserve(static_cast<std::size_t>(names.size()));
    for (const QString& name : names) {
        AutosaveEntry entry;
        if (!readSidecar(dir.absoluteFilePath(name), &entry)) continue;
        // 側錄指向的 PDF 不在了就沒有復原價值，列出來只會讓使用者按下去得到錯誤。
        if (!QFileInfo::exists(QString::fromStdString(entry.autosavePath))) continue;
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool AutosaveManager::discard(const AutosaveEntry& entry) {
    bool removed = false;
    if (!entry.sidecarPath.empty()) {
        removed = QFile::remove(QString::fromStdString(entry.sidecarPath)) || removed;
    }
    if (!entry.autosavePath.empty()) {
        removed = QFile::remove(QString::fromStdString(entry.autosavePath)) || removed;
    }
    return removed;
}

std::string AutosaveManager::autosavePathFor(const std::string& originalPath,
                                             const std::string& directory) {
    const QString dirPath = resolveDirectory(directory);
    return (dirPath + QStringLiteral("/") + keyFor(QString::fromStdString(originalPath)) +
            QLatin1String(kPdfSuffix))
        .toStdString();
}

}  // namespace alioth::engine::save
