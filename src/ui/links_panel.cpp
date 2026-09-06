#include "ui/links_panel.h"

#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/document_controller.h"

namespace alioth::ui {
namespace {

constexpr int kIndexRole = Qt::UserRole + 1;

}  // namespace

LinksPanel::LinksPanel(alioth::app::DocumentController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("linksPanel"));
    setAccessibleName(tr("連結"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    status_ = new QLabel(tr("尚未開啟文件"), this);
    status_->setObjectName(QStringLiteral("linksStatus"));
    status_->setAccessibleName(tr("連結狀態"));
    status_->setWordWrap(true);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("linksTree"));
    tree_->setAccessibleName(tr("連結清單"));
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("類型"), tr("目標")});
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->setRootIsDecorated(false);

    layout->addWidget(status_);
    layout->addWidget(tree_);

    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr || controller_ == nullptr) return;
        const auto& links = controller_->linksForPage(pageIndex_);
        const int index = item->data(0, kIndexRole).toInt();
        if (index < 0 || static_cast<std::size_t>(index) >= links.size()) return;
        // 開啟與否交給呼叫端。外部網址在這裡直接開等於繞過 PRD §8.2 的確認。
        emit linkActivated(links[static_cast<std::size_t>(index)]);
    });
}

void LinksPanel::setPage(int pageIndex) {
    pageIndex_ = pageIndex;
    if (controller_ == nullptr || pageIndex < 0) {
        status_->setText(tr("尚未開啟文件"));
        tree_->clear();
        return;
    }
    status_->setText(tr("正在讀取第 %1 頁的連結").arg(pageIndex + 1));
    tree_->clear();
    controller_->requestLinks(pageIndex);
}

void LinksPanel::linksArrived(int pageIndex) {
    // 晚到的回覆屬於已經離開的那一頁，套上去會讓面板顯示別頁的連結。
    if (pageIndex != pageIndex_) return;
    rebuild();
}

void LinksPanel::rebuild() {
    tree_->clear();
    if (controller_ == nullptr || pageIndex_ < 0) return;

    const auto& links = controller_->linksForPage(pageIndex_);
    int shown = 0;
    for (std::size_t i = 0; i < links.size(); ++i) {
        const domain::LinkTarget& link = links[i];
        // 引擎已經把 Launch action 之類的擋在外面（PRD §8.2），這裡再擋一次
        // 是因為「面板列出一個按了沒反應的項目」比不列出來更讓人困惑。
        if (!link.isValid()) continue;

        auto* item = new QTreeWidgetItem(tree_);
        if (link.isExternal()) {
            item->setText(0, tr("外部網址"));
            // 網址完整顯示。截短的網址正是釣魚連結最好用的偽裝。
            item->setText(1, QString::fromStdString(link.uri));
        } else {
            item->setText(0, tr("跳至頁面"));
            item->setText(1, tr("第 %1 頁").arg(*link.pageIndex + 1));
        }
        item->setData(0, kIndexRole, static_cast<int>(i));
        ++shown;
    }

    status_->setText(shown == 0 ? tr("第 %1 頁沒有連結").arg(pageIndex_ + 1)
                                : tr("第 %1 頁共 %2 個連結").arg(pageIndex_ + 1).arg(shown));
}

}  // namespace alioth::ui
