#include "app/redaction_service.h"

#include <QCryptographicHash>
#include <QFile>

#include <algorithm>
#include <cstring>

#include "engine/objects/incremental_appender.h"
#include "engine/redaction/redaction_applier.h"
#include "engine/redaction/redaction_marks.h"
#include "engine/redaction/sanitizer.h"
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

// 與註解服務同一份邊界守衛：尾端 4 KB 的雜湊。復原前比對，確保這段期間
// 沒有別人改過檔案。
QByteArray boundaryHashOf(const QByteArray& bytes) {
    constexpr int kWindow = 4096;
    const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
    return QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
}

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// 增量寫入的共用收尾：確認輸出真的是「原檔前綴 + 附加段」再落盤。
// 不驗這一步的話，一個把整份重寫的實作也會通過，而既有簽章會從
// 「有效、簽章後有變更」掉成無效——那是 CLAUDE.md 的核心賣點。
HighlightResult finishIncremental(const QString& path, const QByteArray& original,
                                  const engine::objects::BuildResult& built,
                                  const QString& successMessage) {
    HighlightResult result;
    result.previousSize = static_cast<quint64>(original.size());
    result.boundaryGuard = boundaryHashOf(original);

    if (!built.ok) {
        result.message = QObject::tr("增量儲存失敗：%1")
                             .arg(QString::fromStdString(built.diagnostic));
        return result;
    }
    if (built.bytes.size() < static_cast<std::size_t>(original.size()) ||
        std::memcmp(built.bytes.data(), original.constData(),
                    static_cast<std::size_t>(original.size())) != 0) {
        result.message = QObject::tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章");
        return result;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = QObject::tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = successMessage;
    return result;
}

}  // namespace

RedactionService::RedactionService(QObject* parent) : QObject(parent) {}

HighlightResult RedactionService::markArea(const QString& path, std::int32_t pageIndex,
                                           const domain::RectF& area) {
    HighlightResult result;
    const domain::RectF normalized = area.normalized();
    if (normalized.isEmpty()) {
        // 空區域是無效標記，絕不能被當成「整頁」——那會在套用時刪掉一整頁。
        result.message = tr("塗黑範圍是空的");
        return result;
    }

    QByteArray original;
    if (!readAll(path, &original, &result.message)) return result;

    domain::RedactionMark mark;
    mark.pageIndex = pageIndex;
    mark.areas.push_back(normalized);
    domain::RedactionMarkSet marks;
    marks.add(std::move(mark));

    std::string diagnostic;
    const engine::objects::BuildResult built =
        engine::redaction::markRedactions(toStd(original), marks, &diagnostic);
    if (!built.ok && built.diagnostic.empty() && !diagnostic.empty()) {
        result.message = tr("無法標記：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }
    return finishIncremental(path, original, built,
                             tr("已標記待塗黑區域（增量 %1 位元組）").arg(built.appendedBytes));
}

HighlightResult RedactionService::clearMarks(const QString& path, std::int32_t pageIndex) {
    HighlightResult result;
    QByteArray original;
    if (!readAll(path, &original, &result.message)) return result;

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    if (appender.open(toStd(original), &diagnostic) != engine::objects::SourceStatus::Ok) {
        result.message = tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    const engine::redaction::MarkWriteResult removed =
        engine::redaction::removeRedactionMarks(appender, pageIndex);
    if (!removed.ok) {
        result.message = tr("移除標記失敗：%1").arg(QString::fromStdString(removed.diagnostic));
        return result;
    }

    return finishIncremental(path, original, appender.build(),
                             tr("已移除 %1 則塗黑標記").arg(removed.annotationObjects.size()));
}

int RedactionService::pendingMarkCount(const QString& path) const {
    QByteArray original;
    QString ignored;
    if (!readAll(path, &original, &ignored)) return 0;

    engine::objects::IncrementalAppender appender;
    if (appender.open(toStd(original)) != engine::objects::SourceStatus::Ok) return 0;
    return static_cast<int>(engine::redaction::readRedactionMarks(appender.source()).size());
}

PageOperationResult RedactionService::applyMarks(const QString& path, RewriteConsent) {
    PageOperationResult result;
    QByteArray original;
    if (!readAll(path, &original, &result.message)) return result;

    const engine::redaction::ApplyResult applied = engine::redaction::applyMarkedRedactions(
        toStd(original), domain::IrreversibleConsent::confirmed());
    if (!applied.ok) {
        result.message = tr("套用塗黑失敗：%1").arg(QString::fromStdString(applied.diagnostic));
        return result;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(applied.bytes.data(), applied.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.previousBytes = original;
    // 逐項回報刪掉了什麼。只說「已套用」的話，使用者無從判斷該被移除的影像
    // 是不是真的被移除了——而那正是塗黑唯一重要的事。
    result.message = tr("已套用塗黑：移除文字 %1 段、影像 %2 張、註解 %3 則，涵蓋 %4 頁")
                         .arg(applied.stats.removedStrings)
                         .arg(applied.stats.removedImages + applied.stats.removedInlineImages)
                         .arg(applied.stats.removedAnnotations)
                         .arg(applied.stats.pagesTouched);
    return result;
}

PageOperationResult RedactionService::sanitize(const QString& path, RewriteConsent) {
    PageOperationResult result;
    QByteArray original;
    if (!readAll(path, &original, &result.message)) return result;

    const engine::redaction::SanitizeResult sanitized =
        engine::redaction::sanitizeDocument(toStd(original));
    if (!sanitized.ok) {
        result.message = tr("清除中繼資料失敗：%1")
                             .arg(QString::fromStdString(sanitized.diagnostic));
        return result;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(sanitized.bytes.data(), sanitized.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.previousBytes = original;
    // 逐項回報。只說「已清除」的話，使用者無從知道那份檔案本來帶了什麼——
    // 而「原來裡面有三個嵌入檔案」正是他做這件事想知道的資訊。
    result.message = tr("已清除：文件資訊 %1 項、中繼資料串流 %2 個、製作工具資料 %3 筆、"
                        "嵌入檔案 %4 個、JavaScript %5 處")
                         .arg(sanitized.stats.clearedInfoKeys)
                         .arg(sanitized.stats.removedMetadataStreams)
                         .arg(sanitized.stats.removedPieceInfo)
                         .arg(sanitized.stats.removedEmbeddedFiles)
                         .arg(sanitized.stats.removedJavaScript);
    return result;
}

}  // namespace alioth::app
