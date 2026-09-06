#include "ui/ribbon_customize_dialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_default_layout.h"

namespace alioth::ui {
namespace {

// 樹節點的種類。存在 Qt::UserRole 裡，讓「選了什麼」的判斷不必靠深度推測——
// 靠深度的話，某天多一層巢狀就會整個錯亂。
enum class NodeKind { Page, Group, Item };
constexpr int kKindRole = Qt::UserRole;
constexpr int kActionIdRole = Qt::UserRole + 1;
constexpr int kSizeRole = Qt::UserRole + 2;

[[nodiscard]] NodeKind kindOf(const QTreeWidgetItem* item) {
    return static_cast<NodeKind>(item->data(0, kKindRole).toInt());
}

}  // namespace

RibbonCustomizeDialog::RibbonCustomizeDialog(ribbon::Layout layout,
                                             ribbon::ActionRegistry* registry, QWidget* parent)
    : QDialog(parent), layout_(std::move(layout)), registry_(registry) {
    setWindowTitle(tr("自訂 Ribbon"));
    setObjectName(QStringLiteral("ribbonCustomizeDialog"));
    resize(820, 560);

    auto* outer = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("左邊是可以加入的功能，右邊是目前的 Ribbon 結構。"
           "選一個群組再按「加入」，就會把功能放進那個群組。"),
        this);
    hint->setWordWrap(true);
    outer->addWidget(hint);

    auto* columns = new QHBoxLayout;
    outer->addLayout(columns, 1);

    auto* leftColumn = new QVBoxLayout;
    leftColumn->addWidget(new QLabel(tr("可用的功能"), this));
    available_ = new QListWidget(this);
    available_->setObjectName(QStringLiteral("ribbonAvailableActions"));
    available_->setAccessibleName(tr("可用的功能"));
    available_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    leftColumn->addWidget(available_, 1);
    columns->addLayout(leftColumn, 1);

    auto* middleColumn = new QVBoxLayout;
    middleColumn->addStretch(1);
    addButton_ = new QPushButton(tr("加入 →"), this);
    removeButton_ = new QPushButton(tr("← 移除"), this);
    upButton_ = new QPushButton(tr("上移"), this);
    downButton_ = new QPushButton(tr("下移"), this);
    addButton_->setObjectName(QStringLiteral("ribbonCustomizeAdd"));
    removeButton_->setObjectName(QStringLiteral("ribbonCustomizeRemove"));
    upButton_->setObjectName(QStringLiteral("ribbonCustomizeUp"));
    downButton_->setObjectName(QStringLiteral("ribbonCustomizeDown"));
    for (QPushButton* button : {addButton_, removeButton_, upButton_, downButton_}) {
        middleColumn->addWidget(button);
    }
    middleColumn->addStretch(1);
    columns->addLayout(middleColumn);

    auto* rightColumn = new QVBoxLayout;
    rightColumn->addWidget(new QLabel(tr("Ribbon 結構"), this));
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("ribbonStructure"));
    tree_->setAccessibleName(tr("Ribbon 結構"));
    tree_->setHeaderHidden(true);
    rightColumn->addWidget(tree_, 2);

    rightColumn->addWidget(new QLabel(tr("快速存取列"), this));
    quickAccess_ = new QListWidget(this);
    quickAccess_->setObjectName(QStringLiteral("ribbonQuickAccess"));
    quickAccess_->setAccessibleName(tr("快速存取列"));
    rightColumn->addWidget(quickAccess_, 1);
    columns->addLayout(rightColumn, 2);

    auto* buttons = new QDialogButtonBox(this);
    buttons->setObjectName(QStringLiteral("ribbonCustomizeButtons"));
    auto* toQuickAccess = buttons->addButton(tr("加到快速存取列"), QDialogButtonBox::ActionRole);
    toQuickAccess->setObjectName(QStringLiteral("ribbonCustomizeAddQuickAccess"));
    auto* fromQuickAccess =
        buttons->addButton(tr("從快速存取列移除"), QDialogButtonBox::ActionRole);
    fromQuickAccess->setObjectName(QStringLiteral("ribbonCustomizeRemoveQuickAccess"));
    auto* resetButton = buttons->addButton(tr("還原預設"), QDialogButtonBox::ResetRole);
    resetButton->setObjectName(QStringLiteral("ribbonCustomizeReset"));
    buttons->addButton(QDialogButtonBox::Ok);
    buttons->addButton(QDialogButtonBox::Cancel);
    outer->addWidget(buttons);

    connect(addButton_, &QPushButton::clicked, this, &RibbonCustomizeDialog::addSelectedToGroup);
    connect(removeButton_, &QPushButton::clicked, this,
            &RibbonCustomizeDialog::removeSelectedFromTree);
    connect(upButton_, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(downButton_, &QPushButton::clicked, this, [this] { moveSelected(1); });
    connect(toQuickAccess, &QPushButton::clicked, this,
            &RibbonCustomizeDialog::addSelectedToQuickAccess);
    connect(fromQuickAccess, &QPushButton::clicked, this,
            &RibbonCustomizeDialog::removeSelectedFromQuickAccess);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        layout_ = ribbon::defaultLayout();
        rebuildTree();
        rebuildQuickAccess();
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            &RibbonCustomizeDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        collectFromTree();
        accept();
    });

    rebuildAvailable();
    rebuildTree();
    rebuildQuickAccess();
}

QString RibbonCustomizeDialog::displayNameFor(const QString& actionId) const {
    if (registry_ != nullptr) {
        if (const QAction* action = registry_->action(actionId); action != nullptr) {
            // 去掉助憶鍵的 & ——顯示成 "檔案(&F)" 在清單裡只是雜訊。
            QString text = action->text();
            text.remove(QLatin1Char('&'));
            if (!text.isEmpty()) return text;
        }
    }
    return actionId;
}

void RibbonCustomizeDialog::rebuildAvailable() {
    available_->clear();
    if (registry_ == nullptr) return;

    // 只列真的註冊過的動作。列出設定檔裡有、程式裡沒有的 id，使用者可以把一顆
    // 永遠停用的按鈕加進工具列，而它看起來只是「壞掉的按鈕」。
    QStringList ids = registry_->ids();
    std::sort(ids.begin(), ids.end(), [this](const QString& a, const QString& b) {
        return displayNameFor(a).localeAwareCompare(displayNameFor(b)) < 0;
    });
    for (const QString& id : ids) {
        auto* item = new QListWidgetItem(displayNameFor(id), available_);
        item->setData(kActionIdRole, id);
        // id 放 tooltip：同名的動作不少（例如兩個「屬性」），只看名稱分不出來。
        item->setToolTip(id);
    }
}

void RibbonCustomizeDialog::rebuildTree() {
    tree_->clear();
    for (const ribbon::Page& page : layout_.pages) {
        auto* pageItem = new QTreeWidgetItem(tree_);
        pageItem->setText(0, page.title.isEmpty() ? page.id : page.title);
        pageItem->setData(0, kKindRole, static_cast<int>(NodeKind::Page));
        pageItem->setData(0, kActionIdRole, page.id);

        for (const ribbon::Group& group : page.groups) {
            auto* groupItem = new QTreeWidgetItem(pageItem);
            groupItem->setText(0, group.title.isEmpty() ? group.id : group.title);
            groupItem->setData(0, kKindRole, static_cast<int>(NodeKind::Group));
            groupItem->setData(0, kActionIdRole, group.id);

            for (const ribbon::Item& item : group.items) {
                auto* actionItem = new QTreeWidgetItem(groupItem);
                actionItem->setText(0, item.label.isEmpty() ? displayNameFor(item.actionId)
                                                            : item.label);
                actionItem->setData(0, kKindRole, static_cast<int>(NodeKind::Item));
                actionItem->setData(0, kActionIdRole, item.actionId);
                actionItem->setData(0, kSizeRole, static_cast<int>(item.size));
                actionItem->setToolTip(0, item.actionId);
            }
        }
    }
    tree_->expandAll();
    updateButtons();
}

void RibbonCustomizeDialog::rebuildQuickAccess() {
    quickAccess_->clear();
    for (const QString& id : layout_.quickAccessActionIds) {
        auto* item = new QListWidgetItem(displayNameFor(id), quickAccess_);
        item->setData(kActionIdRole, id);
        item->setToolTip(id);
    }
}

void RibbonCustomizeDialog::addSelectedToGroup() {
    QTreeWidgetItem* target = tree_->currentItem();
    if (target == nullptr) return;
    // 選到分頁或功能時，往上／往旁邊找出該放進哪個群組——強迫使用者
    // 精準點在群組上是沒必要的刁難。
    if (kindOf(target) == NodeKind::Item) target = target->parent();
    if (target == nullptr || kindOf(target) != NodeKind::Group) return;

    for (const QListWidgetItem* source : available_->selectedItems()) {
        auto* actionItem = new QTreeWidgetItem(target);
        const QString id = source->data(kActionIdRole).toString();
        actionItem->setText(0, displayNameFor(id));
        actionItem->setData(0, kKindRole, static_cast<int>(NodeKind::Item));
        actionItem->setData(0, kActionIdRole, id);
        actionItem->setData(0, kSizeRole, static_cast<int>(ribbon::ItemSize::Small));
        actionItem->setToolTip(0, id);
    }
    target->setExpanded(true);
}

void RibbonCustomizeDialog::removeSelectedFromTree() {
    QTreeWidgetItem* item = tree_->currentItem();
    // 只允許移除功能，不允許移除分頁或群組。
    //
    // 刪掉一整個分頁是使用者幾乎一定會後悔、而且很難靠「還原預設」以外的方式
    // 復原的操作（那會連其他自訂一起丟掉）。要那個功能就該有獨立的確認流程，
    // 不該掛在同一顆「移除」上。
    if (item == nullptr || kindOf(item) != NodeKind::Item) return;
    delete item;
    updateButtons();
}

void RibbonCustomizeDialog::moveSelected(int delta) {
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || kindOf(item) != NodeKind::Item) return;
    QTreeWidgetItem* parent = item->parent();
    if (parent == nullptr) return;

    const int index = parent->indexOfChild(item);
    const int target = index + delta;
    if (target < 0 || target >= parent->childCount()) return;

    parent->takeChild(index);
    parent->insertChild(target, item);
    tree_->setCurrentItem(item);
}

void RibbonCustomizeDialog::addSelectedToQuickAccess() {
    for (const QListWidgetItem* source : available_->selectedItems()) {
        const QString id = source->data(kActionIdRole).toString();
        // 重複加入同一個動作只會讓工具列出現兩顆一樣的按鈕。
        bool exists = false;
        for (int i = 0; i < quickAccess_->count(); ++i) {
            if (quickAccess_->item(i)->data(kActionIdRole).toString() == id) exists = true;
        }
        if (exists) continue;
        auto* item = new QListWidgetItem(displayNameFor(id), quickAccess_);
        item->setData(kActionIdRole, id);
        item->setToolTip(id);
    }
}

void RibbonCustomizeDialog::removeSelectedFromQuickAccess() {
    delete quickAccess_->takeItem(quickAccess_->currentRow());
}

void RibbonCustomizeDialog::updateButtons() {
    QTreeWidgetItem* item = tree_->currentItem();
    const bool isItem = item != nullptr && kindOf(item) == NodeKind::Item;
    removeButton_->setEnabled(isItem);
    upButton_->setEnabled(isItem);
    downButton_->setEnabled(isItem);
    addButton_->setEnabled(item != nullptr);
}

void RibbonCustomizeDialog::collectFromTree() {
    // 保留原本的 id、keyTip 與尺寸等欄位——樹上只呈現了標題與順序，
    // 直接從樹重建整份資料會把沒顯示的欄位全部清成預設值。
    QList<ribbon::Page> pages;
    for (int p = 0; p < tree_->topLevelItemCount(); ++p) {
        const QTreeWidgetItem* pageItem = tree_->topLevelItem(p);
        const QString pageId = pageItem->data(0, kActionIdRole).toString();

        ribbon::Page page;
        // 從原本的 layout 找回這個分頁，保住 keyTip 之類樹上沒有的欄位。
        const auto existingPage =
            std::find_if(layout_.pages.begin(), layout_.pages.end(),
                         [&pageId](const ribbon::Page& candidate) {
                             return candidate.id == pageId;
                         });
        if (existingPage != layout_.pages.end()) page = *existingPage;
        page.groups.clear();

        for (int g = 0; g < pageItem->childCount(); ++g) {
            const QTreeWidgetItem* groupItem = pageItem->child(g);
            const QString groupId = groupItem->data(0, kActionIdRole).toString();

            ribbon::Group group;
            const ribbon::Group* originalGroup = nullptr;
            if (existingPage != layout_.pages.end()) {
                const auto existingGroup =
                    std::find_if(existingPage->groups.begin(), existingPage->groups.end(),
                                 [&groupId](const ribbon::Group& candidate) {
                                     return candidate.id == groupId;
                                 });
                if (existingGroup != existingPage->groups.end()) {
                    group = *existingGroup;
                    originalGroup = &(*existingGroup);
                }
            }
            group.items.clear();

            for (int i = 0; i < groupItem->childCount(); ++i) {
                const QTreeWidgetItem* actionItem = groupItem->child(i);
                const QString actionId = actionItem->data(0, kActionIdRole).toString();

                // 同一顆按鈕若原本就在這個群組裡，整份沿用——label、iconName、keyTip
                // 這些欄位樹上都沒有呈現，重新組一個空的會把它們清掉。
                ribbon::Item item;
                if (originalGroup != nullptr) {
                    const auto existingItem =
                        std::find_if(originalGroup->items.begin(), originalGroup->items.end(),
                                     [&actionId](const ribbon::Item& candidate) {
                                         return candidate.actionId == actionId;
                                     });
                    if (existingItem != originalGroup->items.end()) item = *existingItem;
                }
                item.actionId = actionId;
                item.type = ribbon::ItemType::Action;
                item.size = static_cast<ribbon::ItemSize>(actionItem->data(0, kSizeRole).toInt());
                // 新加入的按鈕沒有圖示名稱；icon_resolver 用的是同一套 id，直接沿用。
                if (item.iconName.isEmpty() && item.size == ribbon::ItemSize::Large) {
                    item.iconName = actionId;
                }
                group.items.push_back(std::move(item));
            }
            page.groups.push_back(std::move(group));
        }
        pages.push_back(std::move(page));
    }
    layout_.pages = std::move(pages);

    QStringList quickAccess;
    for (int i = 0; i < quickAccess_->count(); ++i) {
        quickAccess << quickAccess_->item(i)->data(kActionIdRole).toString();
    }
    layout_.quickAccessActionIds = std::move(quickAccess);
}

}  // namespace alioth::ui
