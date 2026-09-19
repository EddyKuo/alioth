#pragma once

#include <QSize>
#include <QStyledItemDelegate>

namespace alioth::ui {

// 縮圖格子的尺寸。整份 UI 只有這裡定義，清單的 gridSize 與委派的 sizeHint
// 都從這裡取——兩邊各寫一份必然會在某次調整後對不起來，而症狀是格子之間
// 出現一條用不到的空白，或是可點的範圍比看得到的縮圖小。
QSize thumbnailCellSize();
// 紙張方框的最大尺寸（實際繪製時依頁面長寬比內縮）。
QSize thumbnailPageBox();

// 縮圖清單的繪製。
//
// 自己畫而不是靠 QListWidget 的預設委派，理由是預設委派的項目大小取決於
// **圖示到了沒有**：縮圖是非同步到達的，還沒到的格子只有一行頁碼那麼大，
// 於是可點的範圍與選取框都縮成文字那一小塊，而使用者看到的是一個大格子。
// 「點了沒反應」與「選取框只有一小條」都是同一個原因。
//
// 這個委派回傳固定的 sizeHint，並把整格都畫滿：選取底色鋪滿整格，
// 紙張方框永遠存在（還沒載入時是一張空白紙），頁碼在紙張下方。
class ThumbnailDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit ThumbnailDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
};

}  // namespace alioth::ui
