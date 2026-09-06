#pragma once

// 文件比較結果（PRD-CMP-001）。
//
// 引擎已經算出完整的 DocumentDiff——頁面對應、逐段文字差異、每頁的變更計數。
// 這個對話框只呈現它，不重算任何東西。
//
// 兩個呈現上的決定：
//
//   1. **先看頁面對應，再看該頁的差異**。整份文件的差異段落動輒數千條，
//      平鋪成一張清單沒有人讀得完；而使用者實際的問題是「哪幾頁改了」，
//      那正是 PageDiffSummary 回答的。
//
//   2. **降級狀態一定要顯示**。比對受資源上限保護時結果仍然正確，只是某些
//      區段被整段報成替換而不是最小編輯。不說的話，使用者會以為自己看到的
//      是最精細的比對（SDD §7）。

#include <QDialog>

#include "domain/diff.h"

class QLabel;
class QListWidget;
class QTreeWidget;

namespace alioth::ui {

class CompareDialog : public QDialog {
    Q_OBJECT

public:
    CompareDialog(const domain::DocumentDiff& diff, const QString& oldName,
                  const QString& newName, QWidget* parent = nullptr);

signals:
    // 使用者選了某一頁，請主視窗跳過去。頁碼是**新文件**的（0 起算）；
    // 只存在於舊文件的頁面回報 -1，呼叫端據此不跳頁。
    void pageActivated(int newPageIndex);

private:
    void populate(const domain::DocumentDiff& diff);
    void showRegionsFor(int row);

    const domain::DocumentDiff& diff_;
    QLabel* summary_{nullptr};
    QTreeWidget* pages_{nullptr};
    QListWidget* regions_{nullptr};
};

}  // namespace alioth::ui
