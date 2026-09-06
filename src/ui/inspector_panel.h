#pragma once

// 無障礙檢查器與報告面板（PRD-A11Y-003）。
//
// 資料來自 engine::objects::runAccessibilityCheck，呼叫端負責提供已解析的
// PdfSourceDocument 與 StructTree（通常與 Tags／Order 面板共用同一次檔案讀取，
// 見 main_window.cpp 的 reloadNavigationPanels）。
//
// 面板刻意把「發現的問題」與「涵蓋範圍與限制」放在同一個畫面裡，且後者
// 永遠顯示、不會因為沒有問題而被省略：一份只列問題、不說清楚自己查了什麼、
// 沒查什麼的報告，會讓使用者把「這裡沒列」誤讀成「這裡沒問題」，
// 那正是 CLAUDE.md 對這個面板的要求所要避免的事。

#include <QWidget>

#include "engine/objects/accessibility_checker.h"

class QLabel;
class QTreeWidget;

namespace alioth::ui {

class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);

    void setReport(engine::objects::A11yReport report);
    void clearDocument();

signals:
    // 使用者點了某個具體發現，且該發現有對應頁面（pageIndex >= 0）。
    void findingActivated(int pageIndex);

private:
    void rebuild();

    engine::objects::A11yReport report_;
    bool documentOpen_{false};
    QLabel* summary_{nullptr};
    QTreeWidget* tree_view_{nullptr};
};

}  // namespace alioth::ui
