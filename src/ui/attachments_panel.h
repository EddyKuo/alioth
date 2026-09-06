#pragma once

// Attachments 面板（PRD-ANN-015）。
//
// 附件是不可信輸入裡的不可信輸入。這個面板只做兩件事：列出、另存新檔。
// 沒有「開啟」按鈕，而且這是刻意的——雙擊附件就執行它，正是 PDF 被當成
// 惡意程式載體的主要途徑。使用者要開，先存到他自己選的位置，
// 由作業系統的一般路徑處理。
//
// 檔名一律經過清理後才給存檔對話框當預設值：附件檔名可以是
// "..\\..\\Windows\\System32\\x.dll"。

#include <QWidget>

#include <vector>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace alioth::engine::attachments {
struct Attachment;
}

namespace alioth::ui {

class AttachmentsPanel : public QWidget {
    Q_OBJECT

public:
    explicit AttachmentsPanel(QWidget* parent = nullptr);
    ~AttachmentsPanel() override;

    void setAttachments(std::vector<alioth::engine::attachments::Attachment> attachments);

signals:
    // 使用者要求另存。呼叫端負責取出位元組並寫檔——這個面板不碰檔案系統。
    void saveRequested(int index, const QString& suggestedName);
    // 使用者要求跳到附件註解所在的頁。
    void locateRequested(int pageIndex);

private:
    void rebuild();

    std::vector<alioth::engine::attachments::Attachment> attachments_;
    QLabel* summary_{nullptr};
    QTreeWidget* tree_{nullptr};
    QPushButton* save_{nullptr};
};

}  // namespace alioth::ui
