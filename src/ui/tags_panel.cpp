#include "ui/tags_panel.h"

#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QVariant>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

namespace alioth::ui {
namespace {

using engine::objects::StructNode;
using engine::objects::StructTreeStatus;

constexpr int kPageRole = Qt::UserRole;
constexpr int kObjectNumberRole = Qt::UserRole + 1;
constexpr int kNeedsAltRole = Qt::UserRole + 2;
constexpr int kAltTextRole = Qt::UserRole + 3;

// 標準結構型別的中文說明。認得的才翻譯，認不得的原樣顯示——
// 把未知的型別顯示成「其他」會讓使用者失去判斷依據，而標籤化 PDF
// 允許自訂型別（透過 /RoleMap 對應到標準型別）。
QString describeType(const std::string& type) {
    static const QHash<QString, QString> kNames{
        {QStringLiteral("Document"), QObject::tr("文件")},
        {QStringLiteral("Part"), QObject::tr("部分")},
        {QStringLiteral("Sect"), QObject::tr("章節")},
        {QStringLiteral("Art"), QObject::tr("文章")},
        {QStringLiteral("P"), QObject::tr("段落")},
        {QStringLiteral("H"), QObject::tr("標題")},
        {QStringLiteral("H1"), QObject::tr("標題 1")},
        {QStringLiteral("H2"), QObject::tr("標題 2")},
        {QStringLiteral("H3"), QObject::tr("標題 3")},
        {QStringLiteral("H4"), QObject::tr("標題 4")},
        {QStringLiteral("H5"), QObject::tr("標題 5")},
        {QStringLiteral("H6"), QObject::tr("標題 6")},
        {QStringLiteral("L"), QObject::tr("清單")},
        {QStringLiteral("LI"), QObject::tr("清單項目")},
        {QStringLiteral("Lbl"), QObject::tr("項目標記")},
        {QStringLiteral("LBody"), QObject::tr("項目內容")},
        {QStringLiteral("Table"), QObject::tr("表格")},
        {QStringLiteral("TR"), QObject::tr("表格列")},
        {QStringLiteral("TH"), QObject::tr("表頭儲存格")},
        {QStringLiteral("TD"), QObject::tr("儲存格")},
        {QStringLiteral("Figure"), QObject::tr("圖片")},
        {QStringLiteral("Formula"), QObject::tr("公式")},
        {QStringLiteral("Form"), QObject::tr("表單欄位")},
        {QStringLiteral("Link"), QObject::tr("連結")},
        {QStringLiteral("Span"), QObject::tr("行內範圍")},
        {QStringLiteral("Caption"), QObject::tr("圖說")},
    };
    const QString key = QString::fromStdString(type);
    const auto it = kNames.constFind(key);
    if (it == kNames.constEnd()) return key;
    return QStringLiteral("%1 (%2)").arg(it.value(), key);
}

}  // namespace

TagsPanel::TagsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tagsPanel"));
    setAccessibleName(tr("標籤結構"));
    setAccessibleDescription(tr("標籤化 PDF 的邏輯結構樹，供螢幕閱讀器決定閱讀順序"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("tagsSummary"));
    summary_->setWordWrap(true);
    // 摘要是狀態的唯一出口，螢幕閱讀器必須念得到，所以給它明確的角色名稱。
    summary_->setAccessibleName(tr("標籤結構狀態"));

    tree_view_ = new QTreeWidget(this);
    tree_view_->setObjectName(QStringLiteral("tagsTree"));
    tree_view_->setAccessibleName(tr("標籤結構樹"));
    tree_view_->setAccessibleDescription(
        tr("以樹狀列出文件的結構元素；方向鍵展開與收合，Enter 跳到該元素所在頁"));
    tree_view_->setColumnCount(3);
    tree_view_->setHeaderLabels({tr("結構元素"), tr("頁"), tr("替代文字")});
    tree_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    layout->addWidget(summary_);
    layout->addWidget(tree_view_);

    setFocusProxy(tree_view_);

    const auto activate = [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) return;
        const QVariant page = item->data(0, kPageRole);
        if (page.isValid() && page.toInt() >= 0) emit elementActivated(page.toInt());
    };
    connect(tree_view_, &QTreeWidget::itemActivated, this, activate);

    // 右鍵設定替代文字（PRD-A11Y-004）。只在需要替代文字的型別（Figure／Formula／
    // Form／Link，見 StructNode::needsAlternateText）上提供這個動作——對一般段落
    // 顯示這個選項只會誤導使用者以為段落也需要替代文字。
    tree_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_view_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = tree_view_->itemAt(pos);
        if (item == nullptr || !item->data(0, kNeedsAltRole).toBool()) return;

        QMenu menu(this);
        QAction* editAction = menu.addAction(tr("設定替代文字…"));
        const int objectNumber = item->data(0, kObjectNumberRole).toInt();
        editAction->setEnabled(objectNumber != 0);
        editAction->setToolTip(objectNumber != 0
                                   ? QString()
                                   : tr("這個元素是直接物件，沒有自己的物件編號，"
                                       "無法單獨改寫"));
        if (menu.exec(tree_view_->viewport()->mapToGlobal(pos)) == editAction) {
            emit alternateTextEditRequested(objectNumber, item->data(0, kAltTextRole).toString());
        }
    });

    clearDocument();
}

void TagsPanel::clearDocument() {
    documentOpen_ = false;
    tree_ = {};
    missingAlt_ = 0;
    tree_view_->clear();
    summary_->setText(tr("尚未開啟文件。"));
}

void TagsPanel::setStructTree(engine::objects::StructTree tree) {
    tree_ = std::move(tree);
    documentOpen_ = true;
    rebuild();
}

void TagsPanel::appendNode(const StructNode& node, QTreeWidgetItem* parent) {
    auto* item = parent == nullptr ? new QTreeWidgetItem(tree_view_) : new QTreeWidgetItem(parent);

    QString label = describeType(node.type);
    if (!node.title.empty()) {
        label = tr("%1：%2").arg(label, QString::fromStdString(node.title));
    }
    item->setText(0, label);
    item->setData(0, kPageRole, node.pageIndex);
    item->setData(0, kObjectNumberRole, node.objectNumber);
    item->setData(0, kNeedsAltRole, node.needsAlternateText());
    item->setData(0, kAltTextRole,
                  node.hasAlternateText()
                      ? QString::fromStdString(node.altText.empty() ? node.actualText : node.altText)
                      : QString());
    item->setText(1, node.pageIndex >= 0 ? QString::number(node.pageIndex + 1) : tr("—"));

    if (node.hasAlternateText()) {
        const std::string& alt = node.altText.empty() ? node.actualText : node.altText;
        item->setText(2, QString::fromStdString(alt));
    } else if (node.needsAlternateText()) {
        ++missingAlt_;
        // 缺失以文字標明而不是顏色。這條規則對本面板特別重要：
        // 一個為色盲使用者設計的檢查如果自己靠顏色傳達結論，就自相矛盾。
        item->setText(2, tr("缺少替代文字"));
    } else {
        item->setText(2, tr("—"));
    }

    // 每一列都要有可及性文字：樹狀元件的 UIA 名稱來自欄位文字，
    // 但三欄會被念成不連貫的三段。合併成一句才聽得懂。
    item->setData(0, Qt::AccessibleTextRole,
                  tr("%1，%2，%3").arg(label, item->text(1), item->text(2)));

    for (const StructNode& child : node.children) appendNode(child, item);
}

void TagsPanel::rebuild() {
    tree_view_->clear();
    missingAlt_ = 0;

    if (!documentOpen_) {
        summary_->setText(tr("尚未開啟文件。"));
        return;
    }

    switch (tree_.status) {
        case StructTreeStatus::NoStructTree:
            // 這一句是整個面板的存在理由。措辭刻意講結論而不是現象：
            // 「沒有標籤」是事實，「螢幕閱讀器無法判斷閱讀順序」才是使用者
            // 需要知道的後果。
            summary_->setText(
                tr("這份文件沒有標籤結構。螢幕閱讀器無法判斷正確的閱讀順序，"
                   "圖片也不會有替代文字。"));
            return;
        case StructTreeStatus::Malformed:
            summary_->setText(tr("這份文件的標籤結構無法解析：%1。"
                                 "結構存在但不可用，等同未標籤。")
                                  .arg(QString::fromStdString(tree_.diagnostic)));
            return;
        case StructTreeStatus::Truncated:
        case StructTreeStatus::Ok:
            break;
    }

    for (const StructNode& root : tree_.roots) appendNode(root, nullptr);
    tree_view_->expandToDepth(1);

    QString text = tr("%1 個結構元素").arg(tree_.nodeCount);
    if (!tree_.markedContent) {
        // /StructTreeRoot 有但 /MarkInfo /Marked 為 false：結構樹掛在那裡，
        // 內容卻沒有標記可以對應。輔助技術讀出來的順序不保證正確。
        text += tr("；但 /MarkInfo 未標記為 Marked，閱讀順序不保證正確");
    }
    if (missingAlt_ > 0) {
        text += tr("；%1 個元素缺少替代文字").arg(missingAlt_);
    }
    if (tree_.status == StructTreeStatus::Truncated) {
        text += tr("；結構過大，只顯示前 %1 個元素").arg(tree_.nodeCount);
    }
    summary_->setText(text);
}

}  // namespace alioth::ui
