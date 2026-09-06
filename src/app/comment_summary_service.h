#pragma once

// 註解摘要服務（PRD-ANN-028，WP25）。
//
// PDF-XChange／Acrobat 的「Summarize Comments」支援三種版面：
//   僅摘要       — 只輸出摘要文字，不含原文件
//   文件加摘要   — 原文件每一頁後面插入該頁的摘要頁
//   並排         — 左右分割，左邊原頁面、右邊該頁的摘要
//
// 三種都做。本檔負責的是**文字**那一半：把註解整理成排序好的條目，
// 並依版面組出要排版的文字。真正產生頁面與合成的那一半在
// engine/create（文字排版成 PDF 頁）與 engine/pageops（插入與並排合成），
// 由 PageOperationsService 串起來——這一層刻意不碰檔案，才留得住
// 「摘要規則」可以在沒有 PDF 的情況下單獨測試。
//
// 非 ASCII 的處理沿用既有立場：排版層對畫不出來的字明確失敗，不靜默丟字。

#include <QObject>
#include <QString>

#include <vector>

#include "domain/annotation.h"

namespace alioth::app {

struct CommentSummaryEntry {
    domain::AnnotationSummary source;
    QString displayText;  // 已組好的一行摘要文字，供「僅摘要」版面直接輸出
};

class CommentSummaryService : public QObject {
    Q_OBJECT

public:
    explicit CommentSummaryService(QObject* parent = nullptr);

    // 依頁碼、頁內順序排序後產生摘要條目。排序穩定（std::stable_sort），
    // 相同頁碼、相同 indexOnPage 不會發生時本來就唯一，穩定性是為了
    // 未來若排序鍵擴充（例如加入作者分組）時不必重新驗證既有測試。
    [[nodiscard]] static std::vector<CommentSummaryEntry> build(
        const std::vector<domain::AnnotationSummary>& annotations);

    // 「僅摘要」版面的純文字輸出：一則註解一段，含頁碼、型別、作者、時間、內容。
    [[nodiscard]] static QString renderSummaryOnlyText(const std::vector<CommentSummaryEntry>& entries);

    // 單一頁面的摘要文字，供「文件加摘要」與「並排」兩種版面產生插頁。
    // 該頁沒有註解時回傳空字串——呼叫端據此決定不要插一張空白頁。
    [[nodiscard]] static QString renderPageSummaryText(
        const std::vector<CommentSummaryEntry>& entries, int pageIndex);

    // 有註解的頁碼，由小到大且不重複。「文件加摘要」用它決定要在哪幾頁後面插頁。
    [[nodiscard]] static std::vector<int> pagesWithComments(
        const std::vector<CommentSummaryEntry>& entries);
};

}  // namespace alioth::app
