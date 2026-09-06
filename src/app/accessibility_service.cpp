#include "app/accessibility_service.h"

#include <QCryptographicHash>
#include <QFile>

#include <algorithm>
#include <cstring>

#include "engine/objects/incremental_appender.h"
#include "engine/objects/struct_alt_text_writer.h"
#include "platform/atomic_file.h"

namespace alioth::app {
namespace {

// 與 annotation_service.cpp 的 boundaryHashOf 相同的取法：原檔尾端 4 KB 的雜湊，
// 足以在 100 MB 文件上瞬間完成，而附加式寫入唯一會破壞的就是這個邊界附近的內容。
QByteArray boundaryHashOf(const QByteArray& bytes) {
    constexpr int kWindow = 4096;
    const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
    return QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
}

}  // namespace

AccessibilityService::AccessibilityService(QObject* parent) : QObject(parent) {}

SetAlternateTextResult AccessibilityService::setAlternateText(
    const SetAlternateTextRequest& request) {
    SetAlternateTextResult result;

    QFile source(request.path);
    if (!source.open(QIODevice::ReadOnly)) {
        result.message = tr("無法讀取檔案：%1").arg(source.errorString());
        return result;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    result.previousSize = static_cast<quint64>(bytes.size());
    result.boundaryGuard = boundaryHashOf(bytes);

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    const engine::objects::SourceStatus status =
        appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                      &diagnostic);
    if (status != engine::objects::SourceStatus::Ok) {
        result.message = status == engine::objects::SourceStatus::Encrypted
                             ? tr("加密文件尚不支援加註")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    const engine::objects::AltTextEditStatus write = engine::objects::setAlternateText(
        appender, request.structElementObjectNumber, request.altText.toStdString());
    if (!write.ok) {
        result.message = tr("寫入替代文字失敗：%1").arg(QString::fromStdString(write.diagnostic));
        return result;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }
    const std::string& saved = built.bytes;

    // 與 annotation_service.cpp 相同的二次驗證：原檔位元組必須原封不動，
    // 這是既有數位簽章仍顯示「有效、簽章後有變更」的前提。
    if (saved.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(saved.data(), bytes.constData(), static_cast<std::size_t>(bytes.size())) != 0) {
        result.message = tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章");
        return result;
    }

    platform::AtomicFileWriter writer(request.path);
    if (!writer.begin() || !writer.write(saved.data(), saved.size()) || !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = request.altText.isEmpty()
                         ? tr("已移除替代文字（增量 %1 位元組）").arg(built.appendedBytes)
                         : tr("已設定替代文字（增量 %1 位元組）").arg(built.appendedBytes);
    return result;
}

bool AccessibilityService::revertAppend(const QString& path, quint64 previousSize,
                                        const QByteArray& boundaryGuard, QString* message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (message) *message = tr("無法讀取檔案：%1").arg(file.errorString());
        return false;
    }
    const QByteArray current = file.readAll();
    file.close();

    if (static_cast<quint64>(current.size()) < previousSize) {
        if (message) *message = tr("檔案比復原點還短，已中止");
        return false;
    }

    const QByteArray prefix = current.left(static_cast<int>(previousSize));
    if (boundaryHashOf(prefix) != boundaryGuard) {
        if (message) *message = tr("檔案已被其他程式修改，無法復原這一步");
        return false;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() ||
        !writer.write(prefix.constData(), static_cast<std::size_t>(prefix.size())) ||
        !writer.commit()) {
        if (message) *message = tr("寫檔失敗");
        return false;
    }
    return true;
}

}  // namespace alioth::app
