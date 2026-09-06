#include "app/comment_summary_service.h"

#include <algorithm>

namespace alioth::app {

CommentSummaryService::CommentSummaryService(QObject* parent) : QObject(parent) {}

std::vector<CommentSummaryEntry> CommentSummaryService::build(
    const std::vector<domain::AnnotationSummary>& annotations) {
    std::vector<CommentSummaryEntry> entries;
    entries.reserve(annotations.size());
    for (const domain::AnnotationSummary& summary : annotations) {
        CommentSummaryEntry entry{};
        entry.source = summary;

        QString text = QStringLiteral("[p.%1] %2")
                           .arg(summary.pageIndex + 1)
                           .arg(QString::fromStdString(summary.subtype));
        if (!summary.author.empty()) {
            text += QStringLiteral(" — %1").arg(QString::fromStdString(summary.author));
        }
        if (!summary.modified.empty()) {
            text += QStringLiteral(" (%1)").arg(QString::fromStdString(summary.modified));
        }
        if (!summary.contents.empty()) {
            text += QStringLiteral(": %1").arg(QString::fromStdString(summary.contents));
        }
        entry.displayText = text;
        entries.push_back(std::move(entry));
    }

    // 依頁碼、頁內順序排序：使用者期待摘要跟著文件的閱讀順序走，
    // 而不是註解建立的時間順序（那個順序在多人協作時毫無意義）。
    std::stable_sort(entries.begin(), entries.end(),
                     [](const CommentSummaryEntry& a, const CommentSummaryEntry& b) {
                         if (a.source.pageIndex != b.source.pageIndex) {
                             return a.source.pageIndex < b.source.pageIndex;
                         }
                         return a.source.indexOnPage < b.source.indexOnPage;
                     });
    return entries;
}

QString CommentSummaryService::renderSummaryOnlyText(const std::vector<CommentSummaryEntry>& entries) {
    QString text;
    for (const CommentSummaryEntry& entry : entries) {
        text += entry.displayText;
        text += QLatin1Char('\n');
    }
    return text;
}

QString CommentSummaryService::renderPageSummaryText(
    const std::vector<CommentSummaryEntry>& entries, int pageIndex) {
    QString text;
    int count = 0;
    for (const CommentSummaryEntry& entry : entries) {
        if (entry.source.pageIndex != pageIndex) continue;
        ++count;
        text += entry.displayText;
        text += QLatin1Char('\n');
    }
    // 沒有註解就不該產生任何東西。回一個只有標題的頁面，會讓「文件加摘要」
    // 在沒有註解的頁後面插一張看起來像出錯的空白頁。
    if (count == 0) return {};

    return QStringLiteral("Comments on page %1 (%2)\n\n").arg(pageIndex + 1).arg(count) + text;
}

std::vector<int> CommentSummaryService::pagesWithComments(
    const std::vector<CommentSummaryEntry>& entries) {
    std::vector<int> pages;
    for (const CommentSummaryEntry& entry : entries) {
        // entries 已依頁碼排序（build() 的後置條件），因此只需比對最後一個。
        if (pages.empty() || pages.back() != entry.source.pageIndex) {
            pages.push_back(entry.source.pageIndex);
        }
    }
    return pages;
}

}  // namespace alioth::app
