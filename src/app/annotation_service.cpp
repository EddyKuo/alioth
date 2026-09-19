#include "app/annotation_service.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <cstring>

#include "engine/objects/annotation_note_writer.h"
#include "engine/objects/annotation_reader.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/page_object_editor.h"
#include "platform/atomic_file.h"

namespace alioth::app {
namespace {

domain::PdfDate nowAsPdfDate() {
    const QDateTime now = QDateTime::currentDateTime();
    domain::PdfDate date;
    date.year = now.date().year();
    date.month = now.date().month();
    date.day = now.date().day();
    date.hour = now.time().hour();
    date.minute = now.time().minute();
    date.second = now.time().second();
    return date;
}

// 邊界守衛取的是原檔尾端 4 KB 的雜湊。取尾端而不是整份，是因為要在 100 MB 的文件上
// 也能瞬間完成；而附加式寫入唯一會破壞的就是這個邊界附近的內容。
QByteArray boundaryHashOf(const QByteArray& bytes) {
    constexpr int kWindow = 4096;
    const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
    return QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
}

}  // namespace

AnnotationService::AnnotationService(QObject* parent) : QObject(parent) {}

domain::Annotation AnnotationService::stamped(domain::Annotation annotation,
                                              const QString& author, const QString& contents) {
    annotation.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    annotation.author = author.toStdString();
    if (!contents.isEmpty()) annotation.contents = contents.toStdString();
    annotation.creationDate = nowAsPdfDate();
    annotation.modifiedDate = annotation.creationDate;
    return annotation;
}

HighlightResult AnnotationService::addHighlight(const HighlightRequest& request) {
    HighlightResult result;
    if (request.quads.empty()) {
        result.message = tr("沒有選取任何文字");
        return result;
    }

    domain::TextMarkupGeometry geometry;
    geometry.kind = domain::TextMarkupKind::Highlight;
    geometry.quads = request.quads;

    domain::Annotation annotation;
    annotation.color = request.color;
    annotation.opacity = request.opacity;
    annotation.geometry = std::move(geometry);

    AnnotationRequest generic;
    generic.path = request.path;
    generic.pageIndex = request.pageIndex;
    generic.annotation = stamped(std::move(annotation), request.author, request.contents);
    return addAnnotation(generic);
}

HighlightResult AnnotationService::deleteAnnotation(const QString& path,
                                                   std::int32_t pageIndex,
                                                   std::int32_t indexOnPage) {
    HighlightResult result;

    QFile source(path);
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
                             ? tr("加密文件尚不支援刪除註解")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    engine::objects::PdfRef pageRef{};
    if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) {
        result.message = tr("找不到第 %1 頁").arg(pageIndex + 1);
        return result;
    }

    // 取出這一頁目前的 /Annots，找出第 indexOnPage 個項目的物件編號。
    // 用「第幾個」而不是記住物件編號：列表顯示的就是頁內序號，兩者一致
    // 才不會在文件被別的程式改過之後刪掉不相干的註解。
    const std::vector<int> annots =
        engine::objects::pageAnnotationRefs(appender.source(), pageRef);
    if (indexOnPage < 0 || indexOnPage >= static_cast<std::int32_t>(annots.size())) {
        result.message = tr("這一頁沒有第 %1 則註解").arg(indexOnPage + 1);
        return result;
    }

    const int annotationNumber = annots[static_cast<std::size_t>(indexOnPage)];

    // 這一則的 /Popup 也要一起拿掉。
    //
    // /Popup 本身也是 /Annots 的成員（Acrobat 找不到它的話彈出視窗不會出現），
    // 所以只刪父註解會留下一個 /Parent 指向已不存在物件的孤兒 popup。
    // 那不會崩潰，但它仍然算一則註解——數量對不上，而且部分檢視器會把它
    // 畫成一個空的黃色方塊。
    int popupNumber = 0;
    const engine::objects::PdfObject annotationObject = appender.currentObject(annotationNumber);
    if (const engine::objects::PdfDictionary* dict = annotationObject.asDictionary()) {
        if (const engine::objects::PdfObject* popup = dict->find("Popup");
            popup != nullptr && popup->isRef()) {
            popupNumber = popup->asRef().number;
        }
    }

    const engine::objects::PageEditStatus removed =
        engine::objects::removeFromPageArray(appender, pageRef, "Annots", annotationNumber);
    if (!removed.ok) {
        result.message = tr("刪除失敗：%1").arg(QString::fromStdString(removed.diagnostic));
        return result;
    }

    if (popupNumber != 0) {
        const engine::objects::PageEditStatus popupRemoved =
            engine::objects::removeFromPageArray(appender, pageRef, "Annots", popupNumber);
        if (!popupRemoved.ok) {
            result.message =
                tr("刪除彈出視窗失敗：%1").arg(QString::fromStdString(popupRemoved.diagnostic));
            return result;
        }
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

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = tr("已刪除註解（增量 %1 位元組）").arg(built.appendedBytes);
    return result;
}

HighlightResult AnnotationService::updateAnnotation(const QString& path,
                                                    std::int32_t pageIndex,
                                                    std::int32_t indexOnPage,
                                                    const domain::Annotation& annotation) {
    HighlightResult result;

    QFile source(path);
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
                             ? tr("加密文件尚不支援修改註解屬性")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    engine::objects::PdfRef pageRef{};
    if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) {
        result.message = tr("找不到第 %1 頁").arg(pageIndex + 1);
        return result;
    }

    const std::vector<int> annots =
        engine::objects::pageAnnotationRefs(appender.source(), pageRef);
    if (indexOnPage < 0 || indexOnPage >= static_cast<std::int32_t>(annots.size())) {
        result.message = tr("這一頁沒有第 %1 則註解").arg(indexOnPage + 1);
        return result;
    }
    const int annotationNumber = annots[static_cast<std::size_t>(indexOnPage)];

    int popupNumber = 0;
    const engine::objects::PdfObject existing = appender.currentObject(annotationNumber);
    const engine::objects::PdfDictionary* dict = existing.asDictionary();
    if (dict == nullptr) {
        result.message = tr("這一則註解的結構無法解析");
        return result;
    }
    if (const engine::objects::PdfObject* popup = dict->find("Popup");
        popup != nullptr && popup->isRef()) {
        popupNumber = popup->asRef().number;
    }
    if (const engine::objects::PdfObject* irt = dict->find("IRT"); irt != nullptr) {
        // 回覆註解的 /IRT /RT /StateModel /State 都掛在自己的字典上，而這條
        // 路徑是重建字典——帶不回那些鍵就等於把一條審閱串默默拆掉。
        result.message = tr("這是一則回覆，屬性請在它回覆的那一則上修改");
        return result;
    }

    engine::objects::AnnotationWriteOptions options;
    options.replaceObject = annotationNumber;
    options.reusePopupObject = popupNumber;
    const auto write = engine::objects::writeAnnotation(appender, pageIndex, annotation, options);
    if (!write.ok) {
        result.message = tr("寫入註解失敗：%1").arg(QString::fromStdString(write.diagnostic));
        return result;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }

    // 與其他寫入路徑同一道防線：增量的前提是原檔位元組原封不動，
    // 一旦不成立，既有簽章會從「有效、簽章後有變更」變成「無效」。
    if (built.bytes.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(built.bytes.data(), bytes.constData(),
                    static_cast<std::size_t>(bytes.size())) != 0) {
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
    result.message = tr("已更新註解屬性（增量 %1 位元組）").arg(built.appendedBytes);
    return result;
}

HighlightResult AnnotationService::replyToAnnotation(const QString& path, std::int32_t pageIndex,
                                                     std::int32_t indexOnPage,
                                                     const QString& contents,
                                                     const QString& author, const QString& state) {
    HighlightResult result;
    if (contents.trimmed().isEmpty() && state.isEmpty()) {
        // 既沒有內容也沒有狀態的回覆是一則空註解：清單上多一列，點開什麼都沒有。
        result.message = tr("回覆內容是空的");
        return result;
    }

    QFile source(path);
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
                             ? tr("加密文件尚不支援回覆註解")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    engine::objects::PdfRef pageRef{};
    if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) {
        result.message = tr("找不到第 %1 頁").arg(pageIndex + 1);
        return result;
    }
    const std::vector<int> annots =
        engine::objects::pageAnnotationRefs(appender.source(), pageRef);
    if (indexOnPage < 0 || indexOnPage >= static_cast<std::int32_t>(annots.size())) {
        result.message = tr("這一頁沒有第 %1 則註解").arg(indexOnPage + 1);
        return result;
    }
    const int parent = annots[static_cast<std::size_t>(indexOnPage)];

    // 回覆本身是一則 /Text 註解，位置貼著被回覆者的右上角。它不需要被看見
    // ——Acrobat 把回覆顯示在父註解的視窗裡——但 /Rect 仍然要有效，
    // 否則部分檢視器的命中測試會把它當成整頁大的註解。
    domain::RectF parentRect{0, 0, 20, 20};
    if (const engine::objects::PdfDictionary* dict =
            appender.currentObject(parent).asDictionary()) {
        if (const engine::objects::PdfObject* rect = dict->find("Rect")) {
            if (const engine::objects::PdfArray* array = rect->asArray(); array != nullptr &&
                                                                          array->size() == 4) {
                parentRect = domain::RectF{(*array)[0].asNumber(), (*array)[1].asNumber(),
                                           (*array)[2].asNumber(), (*array)[3].asNumber()}
                                 .normalized();
            }
        }
    }

    domain::Annotation reply;
    reply.rect = domain::RectF{parentRect.right, parentRect.top, parentRect.right + 20.0,
                               parentRect.top + 20.0};
    reply.geometry = domain::TextNoteGeometry{};
    reply.color = domain::ColorRgb{1.0, 0.85, 0.0};

    engine::objects::AnnotationWriteOptions options;
    options.inReplyToObject = parent;
    if (!state.isEmpty()) {
        options.stateModel = std::string("Review");
        options.state = state.toStdString();
    }

    const engine::objects::AnnotationWriteResult written = engine::objects::writeAnnotation(
        appender, pageIndex, stamped(std::move(reply), author, contents), options);
    if (!written.ok) {
        result.message = tr("寫入回覆失敗：%1").arg(QString::fromStdString(written.diagnostic));
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

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = state.isEmpty()
                         ? tr("已加入回覆（增量 %1 位元組）").arg(built.appendedBytes)
                         : tr("已標記為 %1（增量 %2 位元組）").arg(state).arg(built.appendedBytes);
    return result;
}

std::vector<XfdfEntry> AnnotationService::readAnnotationsForExport(const QString& path,
                                                                   QString* error) const {
    const auto fail = [error](const QString& message) {
        if (error != nullptr) *error = message;
        return std::vector<XfdfEntry>{};
    };

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(tr("無法讀取檔案：%1").arg(source.errorString()));
    }
    const QByteArray bytes = source.readAll();
    source.close();

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    const engine::objects::SourceStatus status =
        appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                      &diagnostic);
    if (status != engine::objects::SourceStatus::Ok) {
        return fail(status == engine::objects::SourceStatus::Encrypted
                        ? tr("加密文件尚不支援匯出註解")
                        : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic)));
    }

    std::vector<XfdfEntry> entries;
    for (engine::objects::PageAnnotation& read : engine::objects::readAllAnnotations(appender)) {
        XfdfEntry entry;
        entry.pageIndex = read.pageIndex;
        entry.annotation = std::move(read.annotation);
        entries.push_back(std::move(entry));
    }
    if (error != nullptr) error->clear();
    return entries;
}

std::vector<XfdfEntry> AnnotationService::selectEntries(
    const std::vector<XfdfEntry>& entries,
    const std::vector<domain::AnnotationSummary>& wanted) {
    std::vector<XfdfEntry> selected;
    if (wanted.empty()) return selected;

    // 外框用容差比對：兩條路徑各自把座標轉過一輪浮點運算，逐位元組相等
    // 是個過強的要求，而 0.5 點在畫面上小於一個像素，不可能讓兩則不同的
    // 註解被誤認成同一則。
    constexpr double kTolerance = 0.5;
    const auto sameRect = [](const domain::RectF& lhs, const domain::RectF& rhs) {
        return std::abs(lhs.left - rhs.left) < kTolerance &&
               std::abs(lhs.bottom - rhs.bottom) < kTolerance &&
               std::abs(lhs.right - rhs.right) < kTolerance &&
               std::abs(lhs.top - rhs.top) < kTolerance;
    };

    // 已經配對過的 entry 不再參與比對：兩則完全相同（同頁、同型、同框、
    // 同作者、同內容）的註解是合法的，選了一則就該只匯出一則。
    std::vector<bool> taken(entries.size(), false);
    for (const domain::AnnotationSummary& summary : wanted) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (taken[i]) continue;
            const XfdfEntry& entry = entries[i];
            if (entry.pageIndex != summary.pageIndex) continue;
            if (domain::subtypeName(entry.annotation.type()) != summary.subtype) continue;
            if (!sameRect(entry.annotation.rect, summary.rect)) continue;
            if (entry.annotation.author != summary.author) continue;
            if (entry.annotation.contents != summary.contents) continue;
            selected.push_back(entry);
            taken[i] = true;
            break;
        }
    }
    return selected;
}

HighlightResult AnnotationService::addAnnotations(const QString& path,
                                                 const std::vector<XfdfEntry>& entries) {
    HighlightResult result;
    if (entries.empty()) {
        result.message = tr("沒有可寫入的註解");
        return result;
    }

    QFile source(path);
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
                             ? tr("加密文件尚不支援寫入註解")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    // 全部寫進同一次增量段。逐則各寫一次不只慢，更會讓「匯入 200 則」在
    // 復原堆疊上變成 200 步，而使用者心裡那是一個動作。
    int written = 0;
    int skipped = 0;
    for (const XfdfEntry& entry : entries) {
        // 頁碼超出這份文件的範圍是常見的：註解是從另一份較長的文件匯出的。
        // 靜靜塞到最後一頁會讓標記落在不相干的內容上，跳過並回報才是對的。
        engine::objects::PdfRef pageRef{};
        if (!engine::objects::pageRefAt(appender, entry.pageIndex, pageRef)) {
            ++skipped;
            continue;
        }
        const engine::objects::AnnotationWriteResult write =
            engine::objects::writeAnnotation(appender, entry.pageIndex, entry.annotation);
        if (!write.ok) {
            ++skipped;
            continue;
        }
        ++written;
    }

    if (written == 0) {
        result.message = tr("沒有任何註解可寫入這份文件（%1 則的頁碼超出範圍或不支援）")
                             .arg(skipped);
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

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = skipped == 0
                         ? tr("已寫入 %1 則註解（增量 %2 位元組）")
                               .arg(written)
                               .arg(built.appendedBytes)
                         : tr("已寫入 %1 則註解，跳過 %2 則（增量 %3 位元組）")
                               .arg(written)
                               .arg(skipped)
                               .arg(built.appendedBytes);
    return result;
}

HighlightResult AnnotationService::updateAnnotationContents(const QString& path,
                                                            std::int32_t pageIndex,
                                                            std::int32_t indexOnPage,
                                                            const QString& contents) {
    HighlightResult result;

    QFile source(path);
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
                             ? tr("加密文件尚不支援編輯註釋")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    engine::objects::PdfRef pageRef{};
    if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) {
        result.message = tr("找不到第 %1 頁").arg(pageIndex + 1);
        return result;
    }

    const std::vector<int> annots =
        engine::objects::pageAnnotationRefs(appender.source(), pageRef);
    if (indexOnPage < 0 || indexOnPage >= static_cast<std::int32_t>(annots.size())) {
        result.message = tr("這一頁沒有第 %1 則註解").arg(indexOnPage + 1);
        return result;
    }

    const engine::objects::NoteEditResult edited = engine::objects::setAnnotationContents(
        appender, annots[static_cast<std::size_t>(indexOnPage)], contents.toStdString(),
        domain::toPdfDateString(nowAsPdfDate()));
    if (!edited.ok) {
        result.message = tr("無法修改註釋：%1").arg(QString::fromStdString(edited.diagnostic));
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

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = tr("已更新註釋（增量 %1 位元組）").arg(built.appendedBytes);
    return result;
}

HighlightResult AnnotationService::addAnnotation(const AnnotationRequest& request) {
    HighlightResult result;

    QFile source(request.path);
    if (!source.open(QIODevice::ReadOnly)) {
        result.message = tr("無法讀取檔案：%1").arg(source.errorString());
        return result;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    result.previousSize = static_cast<quint64>(bytes.size());
    result.boundaryGuard = boundaryHashOf(bytes);

    // 走自建的物件層通道而不是 PDFium 的註解 API（ADR-002）。
    // 差別在於 FPDFAnnot_SetAP 建不出 /AP 的 /Resources，螢光筆的 Multiply 混合會遺失；
    // 這條路徑寫得出完整的外觀串流資源字典。
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

    const auto write =
        engine::objects::writeAnnotation(appender, request.pageIndex, request.annotation);
    if (!write.ok) {
        result.message = tr("寫入註解失敗：%1").arg(QString::fromStdString(write.diagnostic));
        return result;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }
    const std::string& saved = built.bytes;

    // 增量儲存的前提是原檔位元組原封不動。附加器內部已驗過一次，這裡再驗一次——
    // 一旦這條不成立，既有的數位簽章會從「有效、簽章後有變更」變成「無效」，
    // 而那是本產品的核心賣點（PRD-SIG-003）。兩道檢查的成本遠低於一次靜默失效。
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
    result.message = tr("已加入註解（增量 %1 位元組）").arg(built.appendedBytes);
    return result;
}

bool AnnotationService::revertAppend(const QString& path, quint64 previousSize,
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
        // 檔案在這段期間被別人改過。硬截會砍掉對方的內容，寧可拒絕。
        if (message) *message = tr("檔案已被其他程式修改，無法復原這一步");
        return false;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(prefix.constData(),
                                         static_cast<std::size_t>(prefix.size())) ||
        !writer.commit()) {
        if (message) *message = tr("寫檔失敗");
        return false;
    }
    return true;
}

}  // namespace alioth::app
