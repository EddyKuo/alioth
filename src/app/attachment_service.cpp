#include "app/attachment_service.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>

#include <algorithm>
#include <cstring>

#include "engine/attachments/attachment_reader.h"
#include "engine/attachments/attachment_writer.h"
#include "engine/objects/incremental_appender.h"
#include "platform/atomic_file.h"

namespace alioth::app {
namespace {

bool readAll(const QString& path, QByteArray* out, QString* message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (message) *message = QObject::tr("無法讀取檔案：%1").arg(file.errorString());
        return false;
    }
    *out = file.readAll();
    return true;
}

QByteArray boundaryHashOf(const QByteArray& bytes) {
    constexpr int kWindow = 4096;
    const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
    return QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
}

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace

AttachmentService::AttachmentService(QObject* parent) : QObject(parent) {}

HighlightResult AttachmentService::addAttachment(const AttachmentRequest& request) {
    HighlightResult result;

    QByteArray payload;
    if (!readAll(request.sourceFile, &payload, &result.message)) return result;
    if (static_cast<std::size_t>(payload.size()) >
        engine::attachments::kMaxAttachmentBytes) {
        result.message = tr("附件超過 %1 MB 的上限")
                             .arg(engine::attachments::kMaxAttachmentBytes / (1024 * 1024));
        return result;
    }

    QByteArray bytes;
    if (!readAll(request.path, &bytes, &result.message)) return result;

    result.previousSize = static_cast<quint64>(bytes.size());
    result.boundaryGuard = boundaryHashOf(bytes);

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    const engine::objects::SourceStatus status = appender.open(toStd(bytes), &diagnostic);
    if (status != engine::objects::SourceStatus::Ok) {
        result.message = status == engine::objects::SourceStatus::Encrypted
                             ? tr("加密文件尚不支援加入附件")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    engine::attachments::AttachmentSpec spec;
    // 檔名經過清理再寫進去：附件名稱之後會成為「另存新檔」的預設檔名，
    // 而一個帶 ../ 的名稱在那裡就是路徑穿越。清理在引擎層也做一次，
    // 這裡先做是為了讓寫進 PDF 的名稱本身就是乾淨的。
    spec.fileName = engine::attachments::sanitizeAttachmentFileName(
        QFileInfo(request.sourceFile).fileName().toStdString());
    spec.description = request.description.toStdString();
    spec.mimeType =
        QMimeDatabase().mimeTypeForFile(request.sourceFile).name().toStdString();
    spec.bytes = toStd(payload);
    spec.pageIndex = request.pageIndex;
    spec.rectPt = request.rectPt.normalized();
    spec.author = request.author.toStdString();

    const engine::attachments::AttachmentWriteResult written =
        engine::attachments::addAttachment(appender, spec);
    if (!written.ok) {
        result.message = tr("加入附件失敗：%1").arg(QString::fromStdString(written.diagnostic));
        return result;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }
    if (built.bytes.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(built.bytes.data(), bytes.constData(),
                    static_cast<std::size_t>(bytes.size())) != 0) {
        result.message = tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章");
        return result;
    }

    platform::AtomicFileWriter writer(request.path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = tr("已加入附件「%1」（增量 %2 位元組）")
                         .arg(QString::fromStdString(spec.fileName))
                         .arg(built.appendedBytes);
    return result;
}

}  // namespace alioth::app
