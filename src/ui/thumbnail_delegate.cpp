#include "ui/thumbnail_delegate.h"

#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QRect>

namespace alioth::ui {
namespace {

// 紙張方框、頁碼那一行、以及格子四周的留白。
//
// 三個數字必須一起看：格子高度 = 上留白 + 紙張 + 間隔 + 頁碼 + 下留白。
// 少算任何一段，頁碼就會被裁掉，或是紙張頂到隔壁格。
constexpr int kPadding = 8;
constexpr int kLabelHeight = 20;
constexpr int kGapBelowPage = 4;
const QSize kPageBox{144, 192};

}  // namespace

QSize thumbnailPageBox() { return kPageBox; }

QSize thumbnailCellSize() {
    return QSize(kPageBox.width() + 2 * kPadding,
                 kPageBox.height() + kGapBelowPage + kLabelHeight + 2 * kPadding);
}

ThumbnailDelegate::ThumbnailDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    // 固定尺寸，與圖示到了沒有無關。這是整個修正的關鍵：項目的大小決定
    // 可點範圍與選取框，取決於內容的話兩者都會跟著縮圖的到達時間變動。
    return thumbnailCellSize();
}

void ThumbnailDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect cell = option.rect.adjusted(2, 2, -2, -2);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);

    // 選取與滑鼠停留的底色鋪滿整格，不是只蓋住縮圖或文字。
    // 使用者判斷「我在哪一頁」靠的是這塊色塊，它必須與格子一樣大。
    const QColor highlight = option.palette.color(QPalette::Highlight);
    if (selected) {
        QColor fill = highlight;
        fill.setAlpha(70);
        painter->fillRect(cell, fill);
        painter->setPen(QPen(highlight, 2));
        painter->drawRect(cell.adjusted(1, 1, -1, -1));
    } else if (hovered) {
        QColor fill = highlight;
        fill.setAlpha(28);
        painter->fillRect(cell, fill);
    }

    // 紙張方框。**沒有縮圖時也要畫**——空白紙與有內容的頁一樣大，
    // 格子的外觀才不會因為載入進度而跳動。
    QRect box(QPoint(0, 0), kPageBox);
    box.moveLeft(cell.left() + (cell.width() - box.width()) / 2);
    box.moveTop(cell.top() + kPadding);

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull()) {
        // 依頁面長寬比內縮，橫式頁不會被拉扁。
        const QSize actual = icon.actualSize(kPageBox);
        if (!actual.isEmpty()) {
            QRect fitted(QPoint(0, 0), actual);
            fitted.moveCenter(box.center());
            box = fitted;
        }
    }

    painter->fillRect(box, Qt::white);
    if (!icon.isNull()) {
        icon.paint(painter, box, Qt::AlignCenter);
    }
    painter->setPen(QPen(option.palette.color(QPalette::Mid), 1));
    painter->drawRect(box.adjusted(0, 0, -1, -1));

    // 頁碼。位置固定在格子底部那一行，不跟著紙張的高度浮動——
    // 混合尺寸的文件裡，橫式頁的紙張比較矮，文字跟著上移會參差不齊。
    const QRect label(cell.left(), cell.bottom() - kPadding - kLabelHeight + 1, cell.width(),
                      kLabelHeight);
    painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
    const QString text = index.data(Qt::DisplayRole).toString();
    painter->drawText(label, Qt::AlignCenter,
                      option.fontMetrics.elidedText(text, Qt::ElideRight, label.width()));

    painter->restore();
}

}  // namespace alioth::ui
