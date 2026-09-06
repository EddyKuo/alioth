#pragma once

// 快捷鍵設定頁（PRD-UI-004）。
//
// 鍵位邏輯全部住在 app::ShortcutScheme（純邏輯、無 GUI，見 tests/uisystem/
// test_shortcut_scheme.cpp）。這一頁只是那個模型的檢視：顯示、收使用者的按鍵、
// 把結果交回模型，自己不判斷任何鍵位規則。
//
// 衝突一律回報而不是靜默覆蓋。靜默讓後者贏的話，使用者會在某一天發現某個功能
// 「按了沒反應」而且查不到原因——這正是 IL-4 要防的那種失敗。

#include <QWidget>

class QKeySequenceEdit;
class QLabel;
class QTreeWidget;

namespace alioth::app {
class ShortcutScheme;
}

namespace alioth::ui {

class ShortcutsPage : public QWidget {
    Q_OBJECT

public:
    explicit ShortcutsPage(alioth::app::ShortcutScheme* scheme, QWidget* parent = nullptr);

signals:
    // 鍵位表變了。呼叫端負責把新的鍵位套到執行中的動作上並持久化——
    // 這一頁不碰 QAction，才不會變成第二個 main_window。
    void schemeChanged();

private:
    void rebuild();
    void applyCurrent();
    void clearCurrent();
    void resetAll();
    void importScheme();
    void exportScheme();
    [[nodiscard]] QString currentActionId() const;

    alioth::app::ShortcutScheme* scheme_{nullptr};
    QTreeWidget* tree_{nullptr};
    QKeySequenceEdit* editor_{nullptr};
    QLabel* status_{nullptr};
};

}  // namespace alioth::ui
