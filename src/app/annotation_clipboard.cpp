#include "app/annotation_clipboard.h"

#include <QMimeData>

#include "app/comment_summary_service.h"

namespace alioth::app {
namespace {

// 一段人看得懂的摘要，給 text/plain 用。
//
// 直接沿用註解摘要那一套（PRD-ANN-028），不另外組一份字串：兩處各自組的話，
// 同一則註解貼到郵件裡與出現在摘要報表裡會長得不一樣，而使用者會以為
// 其中一邊漏了東西。
[[nodiscard]] QString plainTextFor(const std::vector<XfdfEntry>& entries) {
    std::vector<domain::AnnotationSummary> summaries;
    summaries.reserve(entries.size());
    for (const XfdfEntry& entry : entries) {
        domain::AnnotationSummary summary;
        summary.pageIndex = entry.pageIndex;
        summary.subtype = domain::subtypeName(entry.annotation.type());
        summary.author = entry.annotation.author;
        summary.contents = entry.annotation.contents;
        summary.modified = domain::toPdfDateString(entry.annotation.modifiedDate);
        summary.rect = entry.annotation.rect;
        summaries.push_back(std::move(summary));
    }
    return CommentSummaryService::renderSummaryOnlyText(CommentSummaryService::build(summaries));
}

}  // namespace

QString annotationMimeType() {
    return QStringLiteral("application/vnd.alioth.annotations+xfdf");
}

QMimeData* makeAnnotationMimeData(const std::vector<XfdfEntry>& entries,
                                  const QString& sourceFilename) {
    if (entries.empty()) return nullptr;

    auto* mime = new QMimeData;
    const std::string xfdf = exportXfdf(entries, sourceFilename.toStdString());
    mime->setData(annotationMimeType(),
                  QByteArray(xfdf.data(), static_cast<qsizetype>(xfdf.size())));
    mime->setText(plainTextFor(entries));
    return mime;
}

std::vector<XfdfEntry> annotationsFromMimeData(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasFormat(annotationMimeType())) return {};
    const QByteArray payload = mime->data(annotationMimeType());
    // 走與檔案匯入完全相同的解析器：DOCTYPE 拒絕與大小上限都在那裡。
    // 剪貼簿的內容同樣不可信任——任何程式都能放一段 XML 上去宣稱是我們的型別。
    const XfdfImportResult result =
        importXfdf(std::string(payload.constData(), static_cast<std::size_t>(payload.size())));
    if (!result.ok) return {};
    return result.entries;
}

}  // namespace alioth::app
