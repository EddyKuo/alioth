#pragma once

// Tags 面板（PRD-A11Y-001）：標籤化 PDF 的邏輯結構樹。
//
// 這個面板最重要的一項行為不是把樹畫出來，而是**在沒有結構樹時明說**。
// 空面板會被解讀成「這份文件沒問題，只是還沒載入」，而實際結論恰好相反：
// 未標籤的 PDF 對螢幕閱讀器而言只是一堆沒有順序的文字。因此四種狀態
// （未開檔／無標籤／結構損毀／有標籤）各自有不同的文字，不共用空字串。
//
// 資料來自 engine::objects::readStructTree，走 ADR-002 的物件層通道。
// 呈現層不呼叫 PDFium——PDFium 的結構樹 API 也取不到 /Alt 與 /ActualText，
// 而那兩個欄位正是替代文字檢查（PRD-A11Y-004）的全部依據。

#include <QString>
#include <QWidget>

#include "engine/objects/struct_tree_reader.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace alioth::ui {

class TagsPanel : public QWidget {
    Q_OBJECT

public:
    explicit TagsPanel(QWidget* parent = nullptr);

    // 換文件時由呼叫端觸發。面板不自己讀檔：解析整份結構樹要把檔案讀進
    // 記憶體，那件事該由已經持有位元組的呼叫端決定何時做。
    void setStructTree(engine::objects::StructTree tree);
    // 沒有開啟文件時的狀態。與「有文件但沒有標籤」是兩回事。
    void clearDocument();

    [[nodiscard]] const engine::objects::StructTree& structTree() const noexcept { return tree_; }
    // 缺少替代文字的元素數（PRD-A11Y-004）。面板把它顯示在摘要列。
    [[nodiscard]] int missingAlternateTextCount() const noexcept { return missingAlt_; }

signals:
    // 使用者點了某個結構元素，且該元素有對應頁面。
    void elementActivated(int pageIndex);
    // 使用者從右鍵選單要求編輯某個元素的替代文字（PRD-A11Y-004）。
    // structElementObjectNumber 為 0 時代表這個元素是直接物件，呼叫端必須
    // 擋下並提示使用者——見 engine/objects/struct_alt_text_writer.h 的說明。
    void alternateTextEditRequested(int structElementObjectNumber, QString currentAltText);

private:
    void rebuild();
    void appendNode(const engine::objects::StructNode& node, QTreeWidgetItem* parent);

    engine::objects::StructTree tree_;
    QLabel* summary_{nullptr};
    QTreeWidget* tree_view_{nullptr};
    int missingAlt_{0};
    bool documentOpen_{false};
};

}  // namespace alioth::ui
