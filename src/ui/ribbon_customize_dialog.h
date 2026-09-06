#pragma once

// 自訂 Ribbon 與快速存取列（PRD-UI-010、PRD-UI-011）。
//
// 編輯的是 ribbon::Layout 這份資料，不是 widget。Ribbon 本身已經完全由這份
// 資料描述並可序列化，因此這個對話框只做三件事：把資料呈現成樹、讓使用者搬動，
// 再把結果交回去。**不直接改動已經建好的 RibbonBar**——那會讓「取消」變成
// 做不到的事，而且畫面狀態與設定檔會分岔。
//
// 可用動作的清單來自 ActionRegistry：只列真的註冊過的動作。列出設定檔裡有、
// 程式裡沒有的 id，使用者可以把一顆永遠停用的按鈕加進工具列，而它看起來
// 只是「壞掉的按鈕」。

#include <QDialog>

#include "ui/ribbon/ribbon_model.h"

class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace alioth::ui {

namespace ribbon {
class ActionRegistry;
}

class RibbonCustomizeDialog : public QDialog {
    Q_OBJECT

public:
    RibbonCustomizeDialog(ribbon::Layout layout, ribbon::ActionRegistry* registry,
                          QWidget* parent = nullptr);

    // 使用者按下確定後的結果。取消時不要呼叫。
    [[nodiscard]] ribbon::Layout result() const { return layout_; }

private:
    void rebuildTree();
    void rebuildAvailable();
    void rebuildQuickAccess();
    void addSelectedToGroup();
    void removeSelectedFromTree();
    void moveSelected(int delta);
    void addSelectedToQuickAccess();
    void removeSelectedFromQuickAccess();
    void updateButtons();
    // 從樹讀回 layout。使用者的搬動直接反映在樹上，確定時才轉成資料。
    void collectFromTree();

    [[nodiscard]] QString displayNameFor(const QString& actionId) const;

    QTreeWidget* tree_{nullptr};
    QListWidget* available_{nullptr};
    QListWidget* quickAccess_{nullptr};
    QPushButton* addButton_{nullptr};
    QPushButton* removeButton_{nullptr};
    QPushButton* upButton_{nullptr};
    QPushButton* downButton_{nullptr};

    ribbon::Layout layout_;
    ribbon::ActionRegistry* registry_{nullptr};
};

}  // namespace alioth::ui
