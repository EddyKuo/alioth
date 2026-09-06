#include "platform/external_change.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace alioth::platform {
namespace {

constexpr qint64 kTailWindow = 4096;

QByteArray tailHashOf(QFile& file) {
    const qint64 size = file.size();
    const qint64 from = std::max<qint64>(0, size - kTailWindow);
    if (!file.seek(from)) return {};
    const QByteArray tail = file.read(size - from);
    return QCryptographicHash::hash(tail, QCryptographicHash::Sha256);
}

}  // namespace

const char* describe(ExternalChangeStatus status) noexcept {
    switch (status) {
        case ExternalChangeStatus::Unchanged:          return "未變更";
        case ExternalChangeStatus::ModifiedExternally:  return "已被外部修改";
        case ExternalChangeStatus::DeletedExternally:   return "已被外部刪除或搬移";
        case ExternalChangeStatus::Unknown:             return "無法確認（缺少可比對的快照）";
    }
    return "未知狀態";
}

FileSnapshot captureSnapshot(const QString& path) {
    FileSnapshot snapshot;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return snapshot;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return snapshot;

    snapshot.exists = true;
    snapshot.size = file.size();
    snapshot.lastModified = info.lastModified();
    snapshot.tailHash = tailHashOf(file);
    return snapshot;
}

ExternalChangeStatus checkForExternalChange(const QString& path, const FileSnapshot& previous) {
    if (!previous.valid()) return ExternalChangeStatus::Unknown;

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return ExternalChangeStatus::DeletedExternally;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return ExternalChangeStatus::DeletedExternally;

    const qint64 currentSize = file.size();
    if (currentSize != previous.size) return ExternalChangeStatus::ModifiedExternally;

    // 大小相同且修改時間也相同：多數雲端同步資料夾在真的沒變時兩者都不動，
    // 這裡提早回傳可以省下一次雜湊計算。
    if (info.lastModified() == previous.lastModified) return ExternalChangeStatus::Unchanged;

    // 大小相同但修改時間不同：可能是真的改了內容又改回同樣大小，
    // 也可能只是時間戳被同步用戶端碰過。用尾端雜湊做最終判定。
    const QByteArray currentTailHash = tailHashOf(file);
    if (currentTailHash.isEmpty() || previous.tailHash.isEmpty()) {
        // 讀不到雜湊（例如檔案剛好在這個瞬間被鎖住）：保守回報已變更，
        // 不能把「無法確認」誤判成「安全」。
        return ExternalChangeStatus::ModifiedExternally;
    }
    if (currentTailHash != previous.tailHash) return ExternalChangeStatus::ModifiedExternally;

    return ExternalChangeStatus::Unchanged;
}

}  // namespace alioth::platform
