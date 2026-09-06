#include "ui/shortcuts_page.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/uisystem/shortcut_scheme.h"

namespace alioth::ui {
namespace {

constexpr int kActionIdRole = Qt::UserRole + 1;

QString describeConflicts(const std::vector<alioth::app::ShortcutConflict>& conflicts) {
    QStringList lines;
    for (const alioth::app::ShortcutConflict& conflict : conflicts) {
        lines << QStringLiteral("%1（%2）：%3")
                     .arg(conflict.sequence.toString(QKeySequence::NativeText),
                          alioth::app::toString(conflict.context),
                          QStringList(conflict.actionIds.begin(), conflict.actionIds.end())
                              .join(QStringLiteral("、")));
    }
    return lines.join(QStringLiteral("\n"));
}

}  // namespace

ShortcutsPage::ShortcutsPage(alioth::app::ShortcutScheme* scheme, QWidget* parent)
    : QWidget(parent), scheme_(scheme) {
    setObjectName(QStringLiteral("shortcutsPage"));
    setAccessibleName(tr("快捷鍵"));

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("shortcutsTree"));
    tree_->setAccessibleName(tr("快捷鍵清單"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("動作"), tr("情境"), tr("快捷鍵")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setRootIsDecorated(false);

    editor_ = new QKeySequenceEdit(this);
    editor_->setObjectName(QStringLiteral("shortcutsEditor"));
    editor_->setAccessibleName(tr("新的快捷鍵"));

    auto* applyButton = new QPushButton(tr("套用"), this);
    applyButton->setObjectName(QStringLiteral("shortcutsApply"));
    auto* clearButton = new QPushButton(tr("清除"), this);
    clearButton->setObjectName(QStringLiteral("shortcutsClear"));
    auto* resetButton = new QPushButton(tr("全部還原預設"), this);
    resetButton->setObjectName(QStringLiteral("shortcutsReset"));
    auto* importButton = new QPushButton(tr("匯入"), this);
    importButton->setObjectName(QStringLiteral("shortcutsImport"));
    auto* exportButton = new QPushButton(tr("匯出"), this);
    exportButton->setObjectName(QStringLiteral("shortcutsExport"));

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("shortcutsStatus"));
    status_->setAccessibleName(tr("快捷鍵狀態"));
    status_->setWordWrap(true);

    auto* editRow = new QHBoxLayout;
    editRow->addWidget(editor_);
    editRow->addWidget(applyButton);
    editRow->addWidget(clearButton);

    auto* fileRow = new QHBoxLayout;
    fileRow->addWidget(resetButton);
    fileRow->addStretch();
    fileRow->addWidget(importButton);
    fileRow->addWidget(exportButton);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);
    layout->addLayout(editRow);
    layout->addLayout(fileRow);
    layout->addWidget(status_);

    connect(applyButton, &QPushButton::clicked, this, [this] { applyCurrent(); });
    connect(clearButton, &QPushButton::clicked, this, [this] { clearCurrent(); });
    connect(resetButton, &QPushButton::clicked, this, [this] { resetAll(); });
    connect(importButton, &QPushButton::clicked, this, [this] { importScheme(); });
    connect(exportButton, &QPushButton::clicked, this, [this] { exportScheme(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                if (item == nullptr) return;
                editor_->setKeySequence(QKeySequence(item->text(2), QKeySequence::NativeText));
            });

    rebuild();
}

QString ShortcutsPage::currentActionId() const {
    QTreeWidgetItem* item = tree_->currentItem();
    return item == nullptr ? QString() : item->data(0, kActionIdRole).toString();
}

void ShortcutsPage::rebuild() {
    const QString kept = currentActionId();
    tree_->clear();
    if (scheme_ == nullptr) return;

    QTreeWidgetItem* restore = nullptr;
    for (const alioth::app::ShortcutBinding& binding : scheme_->bindings()) {
        auto* item = new QTreeWidgetItem(tree_);
        // 沒有描述時退回 id。空白的第一欄會讓整列看起來是壞的。
        item->setText(0, binding.description.isEmpty() ? binding.actionId : binding.description);
        item->setText(1, alioth::app::toString(binding.context));
        item->setText(2, binding.sequence.toString(QKeySequence::NativeText));
        item->setData(0, kActionIdRole, binding.actionId);
        if (binding.actionId == kept) restore = item;
    }
    if (restore != nullptr) tree_->setCurrentItem(restore);
}

void ShortcutsPage::applyCurrent() {
    const QString actionId = currentActionId();
    if (scheme_ == nullptr || actionId.isEmpty()) {
        status_->setText(tr("請先選一個動作"));
        return;
    }

    const QKeySequence sequence = editor_->keySequence();
    if (sequence.isEmpty()) {
        status_->setText(tr("沒有輸入按鍵。要移除快捷鍵請按「清除」"));
        return;
    }

    switch (scheme_->setBinding(actionId, sequence)) {
        case alioth::app::BindResult::Ok:
            rebuild();
            status_->setText(tr("已設定 %1").arg(sequence.toString(QKeySequence::NativeText)));
            emit schemeChanged();
            return;
        case alioth::app::BindResult::Conflict:
            // 明確拒絕。靜默覆蓋的話，被搶走鍵位的那個功能會變成「按了沒反應」。
            status_->setText(tr("%1 已經被同情境下的其他動作占用，未套用")
                                 .arg(sequence.toString(QKeySequence::NativeText)));
            return;
        case alioth::app::BindResult::UnknownAction:
            status_->setText(tr("找不到這個動作：%1").arg(actionId));
            return;
    }
}

void ShortcutsPage::clearCurrent() {
    const QString actionId = currentActionId();
    if (scheme_ == nullptr || actionId.isEmpty()) {
        status_->setText(tr("請先選一個動作"));
        return;
    }
    scheme_->clearBinding(actionId);
    editor_->clear();
    rebuild();
    status_->setText(tr("已移除快捷鍵"));
    emit schemeChanged();
}

void ShortcutsPage::resetAll() {
    if (scheme_ == nullptr) return;
    const auto answer = QMessageBox::question(
        this, tr("還原快捷鍵"), tr("將把所有快捷鍵打回預設值。要繼續嗎？"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    scheme_->resetToDefaults();
    rebuild();
    status_->setText(tr("已還原為預設鍵位"));
    emit schemeChanged();
}

void ShortcutsPage::importScheme() {
    if (scheme_ == nullptr) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("匯入快捷鍵"), QString(),
                                                      tr("JSON (*.json)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        status_->setText(tr("無法讀取 %1").arg(path));
        return;
    }
    const alioth::app::ShortcutScheme::ImportResult result =
        scheme_->importFromJson(file.readAll());
    if (!result.ok) {
        // 匯入是全有全無。「盡量套用」會讓使用者分不出哪幾個鍵其實沒生效。
        status_->setText(result.error.isEmpty()
                             ? tr("整批未匯入，鍵位互相衝突：\n%1")
                                   .arg(describeConflicts(result.conflicts))
                             : tr("整批未匯入：%1").arg(result.error));
        return;
    }
    rebuild();
    status_->setText(tr("已匯入快捷鍵"));
    emit schemeChanged();
}

void ShortcutsPage::exportScheme() {
    if (scheme_ == nullptr) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("匯出快捷鍵"), QString(),
                                                      tr("JSON (*.json)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        status_->setText(tr("無法寫入 %1").arg(path));
        return;
    }
    const QByteArray json = scheme_->exportToJson();
    file.write(json);
    status_->setText(tr("已匯出至 %1").arg(path));
}

}  // namespace alioth::ui
