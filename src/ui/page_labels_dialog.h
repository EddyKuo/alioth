#pragma once

// 頁面標籤編輯對話框（PRD-PAGE-013）。
//
// 頁面標籤是「這一頁在畫面上叫什麼」，與它的實際索引無關——前言用 i、ii、iii，
// 內文從 1 重新開始，附錄用 A-1。使用者在頁碼框輸入 "iii" 時要跳到那一頁，
// 靠的就是這張表。
//
// 編輯的單位是**範圍**而不是單頁：PDF 的 /PageLabels 就是一串範圍，每段的結束
// 由下一段的起點決定。做成逐頁編輯的介面會讓使用者以為可以只改某一頁，
// 而那在格式上不存在——改了之後它後面所有頁的編號都會跟著變。

#include <QDialog>

#include "domain/page_labels.h"

class QTableWidget;
class QPushButton;

namespace alioth::ui {

class PageLabelsDialog : public QDialog {
    Q_OBJECT

public:
    PageLabelsDialog(domain::PageLabelMap labels, std::int32_t pageCount,
                     QWidget* parent = nullptr);

    // 使用者按下確定後的結果。取消時不要呼叫。
    [[nodiscard]] domain::PageLabelMap result() const;

private:
    void rebuildTable();
    void addRange();
    void removeSelectedRange();
    void updateButtons();
    // 從表格讀回範圍。回傳 false 代表有欄位不合法，diagnostic 說明哪裡不對。
    [[nodiscard]] bool collectRanges(std::vector<domain::PageLabelRange>& out,
                                     QString& diagnostic) const;

    QTableWidget* table_{nullptr};
    QPushButton* removeButton_{nullptr};
    domain::PageLabelMap labels_;
    std::int32_t pageCount_{0};
};

}  // namespace alioth::ui
