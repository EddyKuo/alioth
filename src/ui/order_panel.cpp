#include "ui/order_panel.h"

#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace alioth::ui {
namespace {

using engine::objects::PageOrderItem;
using engine::objects::PageReadingOrder;
using engine::objects::StructTreeStatus;

QString describeItem(const PageOrderItem& item) {
    QString label = QString::fromStdString(item.type);
    if (!item.title.empty()) label += QStringLiteral("：%1").arg(QString::fromStdString(item.title));
    if (label.trimmed().isEmpty()) label = QObject::tr("(未命名)");
    return label;
}

}  // namespace

OrderPanel::OrderPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("orderPanel"));
    setAccessibleName(tr("閱讀順序"));
    setAccessibleDescription(tr("比較這一頁的結構順序與內容輸出順序，找出兩者不一致的地方"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("orderSummary"));
    summary_->setWordWrap(true);
    summary_->setAccessibleName(tr("閱讀順序狀態"));

    tree_view_ = new QTreeWidget(this);
    tree_view_->setObjectName(QStringLiteral("orderTree"));
    tree_view_->setAccessibleName(tr("閱讀順序清單"));
    tree_view_->setAccessibleDescription(
        tr("依結構順序列出這一頁的元素，並列出內容順序與是否一致"));
    tree_view_->setColumnCount(4);
    tree_view_->setHeaderLabels({tr("結構元素"), tr("結構序"), tr("內容序"), tr("狀態")});
    tree_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_view_->setRootIsDecorated(false);  // 這裡的縮排是資訊（見 depth），不是可展開的樹狀關係

    layout->addWidget(summary_);
    layout->addWidget(tree_view_);
    setFocusProxy(tree_view_);

    clearDocument();
}

void OrderPanel::clearDocument() {
    documentOpen_ = false;
    tree_ = {};
    pageIndex_ = -1;
    tree_view_->clear();
    summary_->setText(tr("尚未開啟文件。"));
}

void OrderPanel::setStructTree(engine::objects::StructTree tree) {
    tree_ = std::move(tree);
    documentOpen_ = true;
    rebuild();
}

void OrderPanel::setPageIndex(std::int32_t pageIndex) {
    pageIndex_ = pageIndex;
    if (documentOpen_) rebuild();
}

void OrderPanel::rebuild() {
    tree_view_->clear();

    if (!documentOpen_) {
        summary_->setText(tr("尚未開啟文件。"));
        return;
    }
    if (tree_.status != StructTreeStatus::Ok && tree_.status != StructTreeStatus::Truncated) {
        summary_->setText(
            tr("這份文件沒有可用的標籤結構，無法比對閱讀順序（見 Tags 面板的說明）。"));
        return;
    }
    if (pageIndex_ < 0) {
        summary_->setText(tr("沒有目前頁面。"));
        return;
    }

    const PageReadingOrder order = computePageReadingOrder(tree_, pageIndex_);
    if (order.items.empty()) {
        summary_->setText(tr("這一頁沒有任何結構元素。"));
        return;
    }

    for (std::size_t i = 0; i < order.items.size(); ++i) {
        const PageOrderItem& item = order.items[i];
        auto* row = new QTreeWidgetItem(tree_view_);
        const QString indent = QString(item.depth * 2, QChar(' '));
        row->setText(0, indent + describeItem(item));
        row->setText(1, QString::number(item.structureRank + 1));
        row->setText(2, item.contentRank >= 0 ? QString::number(item.contentRank + 1) : tr("量不到"));

        const bool mismatched =
            std::find(order.mismatchIndices.begin(), order.mismatchIndices.end(),
                     static_cast<int>(i)) != order.mismatchIndices.end();
        // 缺失以文字標明而不是顏色，理由與 Tags 面板相同：對比不足或色盲的使用者
        // 一樣要能分辨哪一列有問題。
        row->setText(3, mismatched ? tr("順序不一致") : (item.contentRank >= 0 ? tr("一致") : tr("—")));

        row->setData(0, Qt::AccessibleTextRole,
                     tr("第 %1 項，%2，結構序 %3，內容序 %4，%5")
                         .arg(i + 1)
                         .arg(describeItem(item))
                         .arg(item.structureRank + 1)
                         .arg(row->text(2), row->text(3)));
    }

    QString text = tr("第 %1 頁，%2 個結構元素").arg(pageIndex_ + 1).arg(order.items.size());
    if (!order.mismatchIndices.empty()) {
        text += tr("；%1 個元素的結構順序與內容順序不一致").arg(order.mismatchIndices.size());
    }
    if (order.unknownContentOrderCount > 0) {
        text += tr("；%1 個元素量不到內容順序，未參與比對").arg(order.unknownContentOrderCount);
    }
    summary_->setText(text);
}

}  // namespace alioth::ui
