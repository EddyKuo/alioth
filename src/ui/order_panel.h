#pragma once

// 閱讀順序（Order）面板（PRD-A11Y-002）。
//
// 只顯示結構順序（Tags 面板已經在做這件事）看不出閱讀順序的問題：結構順序
// 本身沒有對錯可言，錯的是它與內容實際被畫出來的順序不一致。因此這裡把
// 兩條順序軸並排顯示——結構順序（權威，螢幕閱讀器實際會念的次序）與內容
// （MCID）順序（見 engine/objects/reading_order.h 的說明：不是幾何視覺位置，
// 是內容串流輸出時依序遞增的標記內容識別碼）——並把兩者不一致的項目標出來。
//
// 這是逐頁的面板：閱讀順序的比對只在單一頁面內有意義，跨頁的順序由頁碼本身
// 決定，不需要另外比對。

#include <QString>
#include <QWidget>

#include <cstdint>

#include "engine/objects/reading_order.h"
#include "engine/objects/struct_tree_reader.h"

class QLabel;
class QTreeWidget;

namespace alioth::ui {

class OrderPanel : public QWidget {
    Q_OBJECT

public:
    explicit OrderPanel(QWidget* parent = nullptr);

    // 換文件時呼叫；面板不自己讀檔，理由與 TagsPanel 相同。
    void setStructTree(engine::objects::StructTree tree);
    void clearDocument();
    // 換頁時呼叫，重新計算這一頁的順序比對。
    void setPageIndex(std::int32_t pageIndex);

private:
    void rebuild();

    engine::objects::StructTree tree_;
    std::int32_t pageIndex_{-1};
    bool documentOpen_{false};
    QLabel* summary_{nullptr};
    QTreeWidget* tree_view_{nullptr};
};

}  // namespace alioth::ui
