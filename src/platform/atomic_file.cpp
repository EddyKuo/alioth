#include "platform/atomic_file.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <chrono>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace alioth::platform {
namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#ifdef _WIN32
QString describeLastOsError() {
    const DWORD code = ::GetLastError();
    return QStringLiteral("Win32 error %1").arg(static_cast<qulonglong>(code));
}
#endif

// 暫存檔名刻意帶上隨機字尾而非固定名稱：同一份文件可能同時被自動儲存與手動儲存寫出，
// 固定名稱會讓兩者互相截斷對方的暫存檔。
QString makeTemporaryPath(const QString& targetPath) {
    const QFileInfo info(targetPath);
    const quint32 salt = QRandomGenerator::global()->generate();
    return info.absolutePath() + QStringLiteral("/.") + info.fileName() +
           QStringLiteral(".alioth-%1.tmp").arg(salt, 8, 16, QLatin1Char('0'));
}

}  // namespace

struct AtomicFileWriter::Impl {
    QString targetPath;
    QString temporaryPath;
    QFile file;
    QString error;
    qint64 written{0};
    bool committed{false};
};

AtomicFileWriter::AtomicFileWriter(QString targetPath) : impl_(std::make_unique<Impl>()) {
    impl_->targetPath = QDir::cleanPath(std::move(targetPath));
}

AtomicFileWriter::~AtomicFileWriter() {
    abort();
}

bool AtomicFileWriter::begin() {
    if (impl_->file.isOpen()) return true;
    if (impl_->targetPath.isEmpty()) {
        impl_->error = QStringLiteral("目標路徑為空");
        return false;
    }

    const QFileInfo targetInfo(impl_->targetPath);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        impl_->error = QStringLiteral("無法建立目標目錄：") + targetInfo.absolutePath();
        return false;
    }

    impl_->temporaryPath = makeTemporaryPath(impl_->targetPath);
    impl_->file.setFileName(impl_->temporaryPath);
    if (!impl_->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        impl_->error = QStringLiteral("無法建立暫存檔 %1：%2")
                           .arg(impl_->temporaryPath, impl_->file.errorString());
        impl_->temporaryPath.clear();
        return false;
    }
    impl_->written = 0;
    impl_->committed = false;
    return true;
}

bool AtomicFileWriter::copyFrom(const QString& sourcePath) {
    if (!impl_->file.isOpen()) {
        impl_->error = QStringLiteral("尚未 begin()");
        return false;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        impl_->error = QStringLiteral("無法讀取來源檔 %1：%2").arg(sourcePath, source.errorString());
        return false;
    }

    // 1 MB 一塊。整份讀進記憶體對 100 MB 以上的工程圖檔是不可接受的常駐峰值。
    constexpr qint64 kChunk = 1 << 20;
    std::vector<char> buffer(static_cast<std::size_t>(kChunk));
    for (;;) {
        const qint64 read = source.read(buffer.data(), kChunk);
        if (read < 0) {
            impl_->error = QStringLiteral("讀取來源檔失敗：") + source.errorString();
            return false;
        }
        if (read == 0) break;
        if (!write(buffer.data(), static_cast<std::size_t>(read))) return false;
    }
    return true;
}

bool AtomicFileWriter::write(const void* data, std::size_t size) {
    if (!impl_->file.isOpen()) {
        impl_->error = QStringLiteral("尚未 begin()");
        return false;
    }
    if (size == 0) return true;
    const qint64 written = impl_->file.write(static_cast<const char*>(data),
                                             static_cast<qint64>(size));
    if (written != static_cast<qint64>(size)) {
        impl_->error = QStringLiteral("寫入暫存檔失敗：") + impl_->file.errorString();
        return false;
    }
    impl_->written += written;
    return true;
}

bool AtomicFileWriter::commit(CommitTiming* timing) {
    if (!impl_->file.isOpen()) {
        impl_->error = QStringLiteral("尚未 begin()");
        return false;
    }

    const auto syncStart = Clock::now();
    if (!impl_->file.flush()) {
        impl_->error = QStringLiteral("flush 失敗：") + impl_->file.errorString();
        abort();
        return false;
    }
    QString syncError;
    if (!syncFileDescriptor(static_cast<int>(impl_->file.handle()), &syncError)) {
        // 同步失敗就更名，等於把「內容還在快取裡」的風險蓋到原檔上。寧可整筆失敗。
        impl_->error = QStringLiteral("落盤同步失敗：") + syncError;
        abort();
        return false;
    }
    const double syncMs = msSince(syncStart);
    impl_->file.close();

    const auto renameStart = Clock::now();
    QString renameError;
    if (!atomicReplace(impl_->temporaryPath, impl_->targetPath, &renameError)) {
        impl_->error = QStringLiteral("原子更名失敗：") + renameError;
        QFile::remove(impl_->temporaryPath);
        impl_->temporaryPath.clear();
        return false;
    }
    const double renameMs = msSince(renameStart);

    if (timing) {
        timing->syncMs = syncMs;
        timing->renameMs = renameMs;
    }
    impl_->committed = true;
    impl_->temporaryPath.clear();
    return true;
}

void AtomicFileWriter::abort() {
    if (impl_->file.isOpen()) {
        impl_->file.close();
    }
    if (!impl_->committed && !impl_->temporaryPath.isEmpty()) {
        QFile::remove(impl_->temporaryPath);
        impl_->temporaryPath.clear();
    }
}

qint64 AtomicFileWriter::bytesWritten() const noexcept {
    return impl_->written;
}

QString AtomicFileWriter::temporaryPath() const {
    return impl_->temporaryPath;
}

QString AtomicFileWriter::targetPath() const {
    return impl_->targetPath;
}

QString AtomicFileWriter::lastError() const {
    return impl_->error;
}

bool AtomicFileWriter::isOpen() const noexcept {
    return impl_->file.isOpen();
}

bool atomicReplace(const QString& temporaryPath, const QString& targetPath, QString* error) {
#ifdef _WIN32
    const QString nativeTemp = QDir::toNativeSeparators(temporaryPath);
    const QString nativeTarget = QDir::toNativeSeparators(targetPath);
    const std::wstring temp = nativeTemp.toStdWString();
    const std::wstring target = nativeTarget.toStdWString();

    if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // ReplaceFileW 會保留目標檔原有的 ACL、建立時間與資料流。對「不破壞原檔」
        // 這個賣點而言，權限被悄悄重設等同破壞——MoveFileExW 做不到這件事。
        if (::ReplaceFileW(target.c_str(), temp.c_str(), nullptr,
                           REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                           nullptr, nullptr) != 0) {
            return true;
        }
        // 目標位於不支援 ReplaceFile 的檔案系統（部分網路磁碟、雲端同步資料夾）時，
        // 退回 MoveFileExW；它一樣是同磁碟區內的原子更名，只是不保留 ACL。
    }

    if (::MoveFileExW(temp.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    if (error) *error = describeLastOsError();
    return false;
#else
    // POSIX rename(2) 在同一檔案系統內覆蓋既有目標是原子的。
    const QByteArray temp = temporaryPath.toLocal8Bit();
    const QByteArray target = targetPath.toLocal8Bit();
    if (::rename(temp.constData(), target.constData()) == 0) {
        // 更名本身也要持久化，否則斷電後目錄項目可能回到舊狀態。
        const QByteArray dir = QFileInfo(targetPath).absolutePath().toLocal8Bit();
        const int fd = ::open(dir.constData(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
        return true;
    }
    if (error) *error = QStringLiteral("rename 失敗，errno=%1").arg(errno);
    return false;
#endif
}

bool syncFileDescriptor(int fileDescriptor, QString* error) {
    if (fileDescriptor < 0) {
        if (error) *error = QStringLiteral("無效的檔案描述子");
        return false;
    }
#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fileDescriptor));
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) *error = QStringLiteral("_get_osfhandle 失敗");
        return false;
    }
    if (::FlushFileBuffers(handle) == 0) {
        if (error) *error = describeLastOsError();
        return false;
    }
    return true;
#else
    if (::fsync(fileDescriptor) != 0) {
        if (error) *error = QStringLiteral("fsync 失敗，errno=%1").arg(errno);
        return false;
    }
    return true;
#endif
}

}  // namespace alioth::platform
