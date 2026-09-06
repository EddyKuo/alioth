#include "ui/history_panel.h"

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHeaderView>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/history_store.h"

namespace alioth::ui {
namespace {

// 項目上掛的資料。用 Qt::UserRole 而不是自訂 model 是因為這個面板的資料量
// 有上限（HistoryStore::kMaxEntries），撐不到需要虛擬化的規模。
constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kPageRole = Qt::UserRole + 2;
constexpr int kScaleRole = Qt::UserRole + 3;
constexpr int kIsMarkRole = Qt::UserRole + 4;

QString relativeTime(const QDateTime& when) {
    if (!when.isValid()) return QCoreApplication::translate("HistoryPanel", "時間不明");
    const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
    if (seconds < 60) return QCoreApplication::translate("HistoryPanel", "剛剛");
    if (seconds < 3600) {
        return QCoreApplication::translate("HistoryPanel", "%1 分鐘前").arg(seconds / 60);
    }
    if (seconds < 86400) {
        return QCoreApplication::translate("HistoryPanel", "%1 小時前").arg(seconds / 3600);
    }
    if (seconds < 86400 * 7) {
        return QCoreApplication::translate("HistoryPanel", "%1 天前").arg(seconds / 86400);
    }
    return when.toString(QStringLiteral("yyyy-MM-dd"));
}

}  // namespace

HistoryPanel::HistoryPanel(alioth::app::HistoryStore* store, QWidget* parent)
    : QWidget(parent), store_(store) {
    setObjectName(QStringLiteral("historyPanel"));
    setAccessibleName(tr("開啟記錄"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("historySearch"));
    // placeholderText 在 UIA 上不等於名稱：欄位空的時候念得到，一旦使用者
    // 開始打字就消失，螢幕閱讀器隨即失去這個欄位的用途。名稱要另外給。
    search_->setAccessibleName(tr("搜尋開啟記錄"));
    search_->setPlaceholderText(tr("搜尋標題或路徑"));
    search_->setClearButtonEnabled(true);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("historyTree"));
    tree_->setAccessibleName(tr("開啟記錄清單"));
    tree_->setAccessibleDescription(tr("Enter 開啟該文件並回到上次的閱讀位置，Delete 從記錄移除"));
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("文件"), tr("最後開啟")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);

    layout->addWidget(search_);
    layout->addWidget(tree_);

    connect(search_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) { activate(item); });

    // Delete 移除選取項。歷史清單會累積使用者不想再看到的東西（誤開的檔案、
    // 已交付的舊版本），沒有移除手段的話他只能整份清空。
    auto* removeAction = new QAction(tr("從歷史移除"), this);
    removeAction->setShortcut(QKeySequence::Delete);
    removeAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(removeAction, &QAction::triggered, this, [this] { removeSelected(); });
    addAction(removeAction);
    setContextMenuPolicy(Qt::ActionsContextMenu);

    reload();
}

void HistoryPanel::reload() {
    tree_->clear();
    if (store_ == nullptr) return;

    for (const alioth::app::HistoryEntry& entry : store_->entries()) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, entry.title.isEmpty() ? QFileInfo(entry.path).fileName() : entry.title);
        item->setText(1, relativeTime(entry.lastOpened));
        item->setToolTip(0, entry.path);
        item->setData(0, kPathRole, entry.path);
        item->setData(0, kPageRole, entry.lastPageIndex);
        item->setData(0, kScaleRole, entry.lastScale);
        item->setData(0, kIsMarkRole, false);

        if (!entry.fileExists()) {
            // 灰掉但保留。顏色之外還加註文字，理由與簽章面板相同：
            // 顏色不單獨承載意義。
            item->setDisabled(true);
            item->setText(1, tr("找不到檔案"));
        }

        for (const alioth::app::ReadingMark& mark : entry.marks) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, mark.label.isEmpty() ? tr("第 %1 頁").arg(mark.pageIndex + 1)
                                                   : mark.label);
            child->setText(1, tr("第 %1 頁").arg(mark.pageIndex + 1));
            child->setData(0, kPathRole, entry.path);
            child->setData(0, kPageRole, mark.pageIndex);
            child->setData(0, kIsMarkRole, true);
        }
    }
    applyFilter();
}

void HistoryPanel::applyFilter() {
    const QString needle = search_->text();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        const bool hit = needle.isEmpty() ||
                         item->text(0).contains(needle, Qt::CaseInsensitive) ||
                         item->data(0, kPathRole).toString().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!hit);
    }
}

void HistoryPanel::activate(QTreeWidgetItem* item) {
    if (item == nullptr || item->isDisabled()) return;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;

    if (item->data(0, kIsMarkRole).toBool()) {
        emit markRequested(path, item->data(0, kPageRole).toInt());
        return;
    }
    emit openRequested(path, item->data(0, kPageRole).toInt(),
                       item->data(0, kScaleRole).toDouble());
}

void HistoryPanel::removeSelected() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || store_ == nullptr) return;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;

    if (item->data(0, kIsMarkRole).toBool()) {
        store_->removeMark(path, item->data(0, kPageRole).toInt());
    } else {
        store_->remove(path);
    }
    store_->save();
    reload();
}

}  // namespace alioth::ui
