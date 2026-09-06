#pragma once

// History 面板（PRD-NAV-009）與使用者書籤（PRD-NAV-005）。
//
// 面板本身刻意很薄：排序、去重、上限、搜尋全部在 app::HistoryStore，
// 這裡只負責把它畫出來並把使用者的意圖轉成訊號。這樣切的理由是
// 歷史的正確性判準（同一份檔案不重複、檔案不在時仍保留）全部可以在
// 沒有 GUI 的情況下驗證。
//
// 找不到的檔案顯示為灰色而不是移除：使用者要靠標題想起那是什麼，
// 而「昨天還在的東西今天消失了」比「顯示但打不開」更難理解。

#include <QWidget>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace alioth::app {
class HistoryStore;
}

namespace alioth::ui {

class HistoryPanel : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPanel(alioth::app::HistoryStore* store, QWidget* parent = nullptr);

    // 由呼叫端在歷史變動後觸發。面板不自己監看 store——
    // 那會讓「誰在什麼時候重建了清單」變得無法追蹤。
    void reload();

signals:
    // 使用者要求開啟某份文件，並捲到記住的位置。
    void openRequested(const QString& path, int pageIndex, double scale);
    // 使用者點了某份文件底下的書籤。
    void markRequested(const QString& path, int pageIndex);

private:
    void applyFilter();
    void activate(QTreeWidgetItem* item);
    void removeSelected();

    alioth::app::HistoryStore* store_{nullptr};
    QLineEdit* search_{nullptr};
    QTreeWidget* tree_{nullptr};
};

}  // namespace alioth::ui
