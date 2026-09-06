#include "app/annotation_flatten_service.h"

#include <QCryptographicHash>
#include <QFile>

#include <algorithm>
#include <map>
#include <cstring>

#include "engine/objects/annotation_flattener.h"
#include "engine/objects/annotation_reader.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/page_object_editor.h"
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

// 頁面的 /Rotate。只影響便利貼圖示的反向旋轉矩陣，不影響其他註解的座標。
domain::Rotation pageRotationOf(const engine::objects::PdfSourceDocument& source,
                                const engine::objects::PdfRef& page) {
    const engine::objects::PdfObject value =
        source.resolve(source.inheritedPageAttribute(page, "Rotate"));
    if (!value.isNumber()) return domain::Rotation::None;
    // 負值與超過 360 的值都合法（ISO 32000-1 §7.7.3.3 只要求是 90 的倍數）。
    int degrees = static_cast<int>(value.asNumber()) % 360;
    if (degrees < 0) degrees += 360;
    switch (degrees) {
        case 90: return domain::Rotation::Cw90;
        case 180: return domain::Rotation::Cw180;
        case 270: return domain::Rotation::Cw270;
        default: return domain::Rotation::None;
    }
}

}  // namespace

AnnotationFlattenService::AnnotationFlattenService(QObject* parent) : QObject(parent) {}

AnnotationFlattenService::FlattenSummary AnnotationFlattenService::preview(
    const QString& path) const {
    FlattenSummary summary;
    QByteArray original;
    QString ignored;
    if (!readAll(path, &original, &ignored)) return summary;

    engine::objects::IncrementalAppender appender;
    if (appender.open(toStd(original)) != engine::objects::SourceStatus::Ok) return summary;

    summary.flattened = static_cast<int>(engine::objects::readAllAnnotations(appender).size());

    // 略過的則數＝/Annots 裡的總數減去讀得回模型的則數。直接數 /Annots 是
    // 唯一能發現「有東西攤不平」的方法——readAllAnnotations 不會為跳過的
    // 項目留位置。
    int total = 0;
    for (int pageIndex = 0;; ++pageIndex) {
        engine::objects::PdfRef pageRef{};
        if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) break;
        total += static_cast<int>(
            engine::objects::pageAnnotationRefs(appender.source(), pageRef).size());
    }
    summary.skipped = std::max(0, total - summary.flattened);
    return summary;
}

HighlightResult AnnotationFlattenService::flattenAll(const QString& path,
                                                     domain::IrreversibleConsent consent) {
    HighlightResult result;
    QByteArray original;
    if (!readAll(path, &original, &result.message)) return result;

    result.previousSize = static_cast<quint64>(original.size());
    result.boundaryGuard = boundaryHashOf(original);

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    if (appender.open(toStd(original), &diagnostic) != engine::objects::SourceStatus::Ok) {
        result.message = tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    const std::vector<engine::objects::PageAnnotation> annotations =
        engine::objects::readAllAnnotations(appender);
    if (annotations.empty()) {
        // 沒有東西可攤平時不要寫檔。寫一個空的附加段會讓檔案變大、既有簽章
        // 從「有效」掉成「有效，簽章後有變更」，而使用者什麼都沒得到。
        result.message = tr("這份文件沒有可攤平的註解");
        return result;
    }

    // /Annots 的參照要先收集起來再開始改。攤平會把項目從陣列裡拿掉，
    // 邊走邊查會讓後面幾則的索引全部位移一格，結果是攤平了 A、摘掉了 B。
    std::map<int, std::vector<int>> refsByPage;
    for (int pageIndex = 0;; ++pageIndex) {
        engine::objects::PdfRef pageRef{};
        if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) break;
        refsByPage[pageIndex] = engine::objects::pageAnnotationRefs(appender.source(), pageRef);
    }

    int flattened = 0;
    for (const engine::objects::PageAnnotation& item : annotations) {
        const std::vector<int>& refs = refsByPage[item.pageIndex];
        if (item.indexOnPage < 0 || static_cast<std::size_t>(item.indexOnPage) >= refs.size()) {
            result.message = tr("註解索引與文件對不上，已中止（第 %1 頁）").arg(item.pageIndex + 1);
            return result;
        }

        engine::objects::PdfRef pageRef{};
        if (!engine::objects::pageRefAt(appender, item.pageIndex, pageRef)) {
            result.message = tr("頁碼超出範圍：%1").arg(item.pageIndex + 1);
            return result;
        }

        engine::objects::FlattenOptions options;
        options.pageRotation = pageRotationOf(appender.source(), pageRef);

        const engine::objects::FlattenResult flattenResult =
            engine::objects::flattenAndDetachAnnotation(appender, item.pageIndex, item.annotation,
                                                        refs[static_cast<std::size_t>(
                                                            item.indexOnPage)],
                                                        consent, options);
        if (!flattenResult.ok) {
            // 中止即整批不套用：附加段還沒落盤，原檔一個位元組都沒動。
            // 「攤平了一半」比完全沒攤平糟得多——使用者看不出停在哪裡。
            result.message = tr("攤平失敗，已整批取消：%1")
                                 .arg(QString::fromStdString(flattenResult.diagnostic));
            return result;
        }
        ++flattened;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message =
            tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }
    if (built.bytes.size() < static_cast<std::size_t>(original.size()) ||
        std::memcmp(built.bytes.data(), original.constData(),
                    static_cast<std::size_t>(original.size())) != 0) {
        result.message = tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章");
        return result;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = tr("已攤平 %1 則註解").arg(flattened);
    return result;
}

}  // namespace alioth::app
