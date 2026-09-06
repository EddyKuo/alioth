#include "engine/save/incremental_saver.h"

#include "engine/pdfium_lock.h"

#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdfview.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#include "platform/atomic_file.h"

namespace alioth::engine::save {
namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

FPDF_DOCUMENT toDocument(DocumentHandle handle) {
    return static_cast<FPDF_DOCUMENT>(handle);
}

// 前綴比對取樣長度。存檔輸出可能是 100 MB，逐位元組比對等於把存檔時間乘二；
// 4 KB 足以涵蓋檔頭與第一批物件，PDFium 若改成重寫整份，這段一定會變。
// 完整的逐位元組比對留在單元測試裡做。
constexpr std::size_t kPrefixProbeBytes = 4096;

// PDFium 的輸出可能是 100 MB 起跳，直接串進暫存檔，不進記憶體。
struct FileSink {
    FPDF_FILEWRITE base{};  // 必須是第一個成員：PDFium 回呼傳回的就是這個位址
    platform::AtomicFileWriter* writer{nullptr};
    std::vector<unsigned char> prefix;  // 供簽章保全檢查用的開頭取樣
    bool failed{false};
};

int writeToFile(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* sink = reinterpret_cast<FileSink*>(self);
    if (!sink->writer || sink->failed) return 0;
    if (sink->prefix.size() < kPrefixProbeBytes && size > 0) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        const std::size_t take =
            std::min(static_cast<std::size_t>(size), kPrefixProbeBytes - sink->prefix.size());
        sink->prefix.insert(sink->prefix.end(), bytes, bytes + take);
    }
    if (!sink->writer->write(data, static_cast<std::size_t>(size))) {
        sink->failed = true;
        return 0;
    }
    return 1;
}

// 原檔的開頭取樣。讀不到就當成無法確認，交由呼叫端保守處理。
std::vector<unsigned char> readPrefix(const QString& path, std::size_t maxBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = file.read(static_cast<qint64>(maxBytes));
    const auto* begin = reinterpret_cast<const unsigned char*>(head.constData());
    return {begin, begin + head.size()};
}

int resolveVersion(FPDF_DOCUMENT document, int requested) {
    int original = 0;
    if (!FPDF_GetFileVersion(document, &original) || original <= 0) {
        // 版本讀不出來時退到 1.7：往上取整不會讓任何既有內容失去意義，
        // 往下猜則可能寫出宣稱 1.4 卻含 1.7 特性的檔案。
        original = 17;
    }
    return std::max(requested, original);
}

FPDF_DWORD flagsFor(bool incremental, const SaveOptions& options) {
    FPDF_DWORD flags = incremental ? FPDF_INCREMENTAL : FPDF_NO_INCREMENTAL;
    if (options.removeSecurity) flags |= FPDF_REMOVE_SECURITY;
    return flags;
}

// 目標可寫性要在動工前判斷。等到更名那一刻才失敗，使用者已經白等了一次完整存檔，
// 而 PRD-IO-005 要求唯讀時自動改走另存——那個決策需要提早知道結果。
bool targetIsWritable(const QString& targetPath, QString* reason) {
    const QFileInfo info(targetPath);
    if (info.exists()) {
        if (!info.isFile()) {
            if (reason) *reason = QStringLiteral("目標不是一般檔案");
            return false;
        }
        if (!info.isWritable()) {
            if (reason) *reason = QStringLiteral("目標檔為唯讀");
            return false;
        }
    }
    const QFileInfo dir(info.absolutePath());
    if (dir.exists() && !dir.isWritable()) {
        if (reason) *reason = QStringLiteral("目標目錄為唯讀");
        return false;
    }
    return true;
}

SaveResult failure(SaveStatus status, const QString& message, SaveMetrics metrics,
                   Clock::time_point start) {
    SaveResult result;
    result.status = status;
    result.message = message.toStdString();
    result.metrics = metrics;
    result.metrics.totalMs = msSince(start);
    return result;
}

}  // namespace

const char* describe(SaveStatus status) noexcept {
    switch (status) {
        case SaveStatus::Ok:                       return "成功";
        case SaveStatus::InvalidDocument:          return "文件把手無效";
        case SaveStatus::SourceUnreadable:         return "原始檔案無法讀取";
        case SaveStatus::TargetNotWritable:        return "目標不可寫入";
        case SaveStatus::TemporaryFileFailed:      return "無法建立暫存檔";
        case SaveStatus::PdfiumWriteFailed:        return "PDFium 產生輸出失敗";
        case SaveStatus::CommitFailed:             return "落盤或原子更名失敗";
        case SaveStatus::RefusedOverwriteOriginal: return "拒絕覆蓋原始檔案";
    }
    return "未知狀態";
}

SaveResult IncrementalSaver::saveIncremental(DocumentHandle document,
                                             const std::string& sourcePath,
                                             const std::string& targetPath,
                                             const SaveOptions& options) {
    // 行程級序列化（ADR-005）。存檔與檢視器渲染是兩條不同的執行緒，
    // 而「檢視器開著文件時存檔」正是存檔的唯一情境。
    //
    // 粒度是一次存檔。PRD 給的預算是 100 MB 文件 300 毫秒——那段期間圖磚會等，
    // 但文件正在被寫，本來就不該同時去渲染它。
    const PdfiumGuard guard;

    const auto start = Clock::now();
    SaveMetrics metrics;

    FPDF_DOCUMENT doc = toDocument(document);
    if (!doc) {
        return failure(SaveStatus::InvalidDocument, QStringLiteral("文件把手為空"), metrics, start);
    }

    const QString source = QString::fromStdString(sourcePath);
    const QString target = QString::fromStdString(targetPath);
    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isFile() || !sourceInfo.isReadable()) {
        return failure(SaveStatus::SourceUnreadable,
                       QStringLiteral("原始檔案無法讀取：") + source, metrics, start);
    }
    metrics.sourceBytes = static_cast<std::uint64_t>(sourceInfo.size());

    QString reason;
    if (!targetIsWritable(target, &reason)) {
        return failure(SaveStatus::TargetNotWritable, reason, metrics, start);
    }

    SaveResult result;
    result.fileVersion = resolveVersion(doc, options.fileVersion);

    platform::AtomicFileWriter writer(target);
    if (!writer.begin()) {
        return failure(SaveStatus::TemporaryFileFailed, writer.lastError(), metrics, start);
    }

    FileSink sink;
    sink.base.version = 1;
    sink.base.WriteBlock = &writeToFile;
    sink.writer = &writer;
    sink.prefix.reserve(kPrefixProbeBytes);

    const auto pdfiumStart = Clock::now();
    const bool written = FPDF_SaveWithVersion(doc, &sink.base, flagsFor(true, options),
                                              result.fileVersion) != 0;
    metrics.pdfiumMs = msSince(pdfiumStart);
    if (!written || sink.failed) {
        return failure(sink.failed ? SaveStatus::TemporaryFileFailed
                                   : SaveStatus::PdfiumWriteFailed,
                       sink.failed ? writer.lastError()
                                   : QStringLiteral("FPDF_SaveWithVersion 回報失敗"),
                       metrics, start);
    }
    metrics.totalBytes = static_cast<std::uint64_t>(writer.bytesWritten());

    // 簽章保全的實質檢查：輸出必須是「原檔位元組 + 追加內容」。
    // 前綴對不上或輸出反而變短，就代表 PDFium 重寫了整份，簽章已經失效。
    const std::vector<unsigned char> sourcePrefix = readPrefix(source, kPrefixProbeBytes);
    const bool prefixPreserved =
        !sourcePrefix.empty() && sink.prefix.size() >= sourcePrefix.size() &&
        std::memcmp(sink.prefix.data(), sourcePrefix.data(), sourcePrefix.size()) == 0;
    result.fullRewriteFallback =
        !prefixPreserved || metrics.totalBytes < metrics.sourceBytes;
    if (!result.fullRewriteFallback) {
        metrics.incrementalBytes = metrics.totalBytes - metrics.sourceBytes;
    }

    platform::CommitTiming timing;
    if (!writer.commit(&timing)) {
        return failure(SaveStatus::CommitFailed, writer.lastError(), metrics, start);
    }
    metrics.syncMs = timing.syncMs;
    metrics.renameMs = timing.renameMs;
    metrics.totalMs = msSince(start);

    result.metrics = metrics;
    return result;
}

SaveResult IncrementalSaver::saveAsCopy(DocumentHandle document, const std::string& targetPath,
                                        const SaveOptions& options) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005），理由同 saveIncremental
    const auto start = Clock::now();
    SaveMetrics metrics;

    FPDF_DOCUMENT doc = toDocument(document);
    if (!doc) {
        return failure(SaveStatus::InvalidDocument, QStringLiteral("文件把手為空"), metrics, start);
    }

    const QString target = QString::fromStdString(targetPath);
    QString reason;
    if (!targetIsWritable(target, &reason)) {
        return failure(SaveStatus::TargetNotWritable, reason, metrics, start);
    }

    SaveResult result;
    result.fileVersion = resolveVersion(doc, options.fileVersion);

    platform::AtomicFileWriter writer(target);
    if (!writer.begin()) {
        return failure(SaveStatus::TemporaryFileFailed, writer.lastError(), metrics, start);
    }

    FileSink sink;
    sink.base.version = 1;
    sink.base.WriteBlock = &writeToFile;
    sink.writer = &writer;

    const auto pdfiumStart = Clock::now();
    const bool written = FPDF_SaveWithVersion(doc, &sink.base, flagsFor(false, options),
                                              result.fileVersion) != 0;
    metrics.pdfiumMs = msSince(pdfiumStart);
    if (!written || sink.failed) {
        return failure(SaveStatus::PdfiumWriteFailed,
                       sink.failed ? writer.lastError()
                                   : QStringLiteral("FPDF_SaveWithVersion 回報失敗"),
                       metrics, start);
    }
    metrics.totalBytes = static_cast<std::uint64_t>(writer.bytesWritten());

    platform::CommitTiming timing;
    if (!writer.commit(&timing)) {
        return failure(SaveStatus::CommitFailed, writer.lastError(), metrics, start);
    }
    metrics.syncMs = timing.syncMs;
    metrics.renameMs = timing.renameMs;
    metrics.totalMs = msSince(start);

    result.metrics = metrics;
    return result;
}

ScopedDocument::~ScopedDocument() {
    close();
}

ScopedDocument::ScopedDocument(ScopedDocument&& other) noexcept
    : runtime_(std::move(other.runtime_)),
      document_(std::exchange(other.document_, nullptr)),
      bytes_(std::move(other.bytes_)) {}

ScopedDocument& ScopedDocument::operator=(ScopedDocument&& other) noexcept {
    if (this != &other) {
        close();
        runtime_ = std::move(other.runtime_);
        document_ = std::exchange(other.document_, nullptr);
        bytes_ = std::move(other.bytes_);
    }
    return *this;
}

bool ScopedDocument::open(const std::string& path, const std::string& password) {
    close();
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）

    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray content = file.readAll();
    file.close();
    if (content.isEmpty()) return false;

    bytes_.assign(reinterpret_cast<const unsigned char*>(content.constData()),
                  reinterpret_cast<const unsigned char*>(content.constData()) + content.size());

    runtime_ = std::make_unique<PdfiumRuntime>();
    document_ = FPDF_LoadMemDocument64(bytes_.data(), bytes_.size(),
                                       password.empty() ? nullptr : password.c_str());
    if (!document_) {
        runtime_.reset();
        bytes_.clear();
        bytes_.shrink_to_fit();
        return false;
    }
    return true;
}

void ScopedDocument::close() {
    if (document_) {
        FPDF_CloseDocument(toDocument(document_));
        document_ = nullptr;
    }
    bytes_.clear();
    bytes_.shrink_to_fit();
    // 守衛必須最後才放掉：文件關閉前 PDFium 不能被銷毀。
    runtime_.reset();
}

int ScopedDocument::pageCount() const {
    return document_ ? FPDF_GetPageCount(toDocument(document_)) : 0;
}

int ScopedDocument::fileVersion() const {
    if (!document_) return 0;
    int version = 0;
    return FPDF_GetFileVersion(toDocument(document_), &version) ? version : 0;
}

int ScopedDocument::signatureCount() const {
    return document_ ? FPDF_GetSignatureCount(toDocument(document_)) : 0;
}

}  // namespace alioth::engine::save
