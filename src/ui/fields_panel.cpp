#include "ui/fields_panel.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "engine/formbuild/field_tree.h"

namespace alioth::ui {
namespace {

using alioth::engine::formbuild::BuildFieldType;
using alioth::engine::formbuild::FieldSummary;
using alioth::engine::formbuild::FieldTreeNode;

constexpr int kNameRole = Qt::UserRole + 1;
constexpr int kPageRole = Qt::UserRole + 2;
constexpr int kIsFieldRole = Qt::UserRole + 3;

QString typeLabel(BuildFieldType type) {
    switch (type) {
        case BuildFieldType::Text:       return QCoreApplication::translate("FieldsPanel", "文字");
        case BuildFieldType::CheckBox:   return QCoreApplication::translate("FieldsPanel", "核取");
        case BuildFieldType::RadioGroup: return QCoreApplication::translate("FieldsPanel", "單選");
        case BuildFieldType::ComboBox:   return QCoreApplication::translate("FieldsPanel", "下拉");
        case BuildFieldType::ListBox:    return QCoreApplication::translate("FieldsPanel", "清單");
        case BuildFieldType::PushButton: return QCoreApplication::translate("FieldsPanel", "按鈕");
        case BuildFieldType::Date:       return QCoreApplication::translate("FieldsPanel", "日期");
        case BuildFieldType::Image:      return QCoreApplication::translate("FieldsPanel", "影像");
        case BuildFieldType::Signature:  return QCoreApplication::translate("FieldsPanel", "簽章");
        case BuildFieldType::Barcode:    return QCoreApplication::translate("FieldsPanel", "條碼");
    }
    return QCoreApplication::translate("FieldsPanel", "未知");
}

}  // namespace

FieldsPanel::FieldsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fieldsPanel"));
    setAccessibleName(tr("表單欄位"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("fieldsSearch"));
    search_->setAccessibleName(tr("搜尋表單欄位"));
    search_->setPlaceholderText(tr("搜尋欄位名稱"));
    search_->setClearButtonEnabled(true);

    currentPageOnly_ = new QCheckBox(tr("只顯示本頁欄位"), this);
    currentPageOnly_->setObjectName(QStringLiteral("fieldsCurrentPageOnly"));

    summary_ = new QLabel(tr("這份文件沒有表單欄位"), this);
    summary_->setObjectName(QStringLiteral("fieldsSummary"));
    summary_->setAccessibleName(tr("表單欄位摘要"));
    summary_->setWordWrap(true);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("fieldsTree"));
    tree_->setAccessibleName(tr("表單欄位清單"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("名稱"), tr("類型"), tr("頁")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    layout->addWidget(search_);
    layout->addWidget(currentPageOnly_);
    layout->addWidget(summary_);
    layout->addWidget(tree_);

    connect(search_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(currentPageOnly_, &QCheckBox::toggled, this, [this] { rebuild(); });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr || !item->data(0, kIsFieldRole).toBool()) return;
        emit fieldActivated(item->data(0, kNameRole).toString(), item->data(0, kPageRole).toInt());
    });
}

FieldsPanel::~FieldsPanel() = default;

void FieldsPanel::setFields(std::vector<FieldSummary> fields) {
    fields_ = std::move(fields);
    rebuild();
}

void FieldsPanel::setCurrentPage(std::int32_t pageIndex) {
    currentPage_ = pageIndex;
    if (currentPageOnly_->isChecked()) rebuild();
}

void FieldsPanel::rebuild() {
    tree_->clear();

    std::vector<FieldSummary> visible = fields_;
    if (currentPageOnly_->isChecked() && currentPage_ >= 0) {
        visible = alioth::engine::formbuild::fieldsOnPage(visible, currentPage_);
    }
    const QString needle = search_->text();
    if (!needle.isEmpty()) {
        std::vector<FieldSummary> filtered;
        for (const FieldSummary& field : visible) {
            if (QString::fromStdString(field.name).contains(needle, Qt::CaseInsensitive)) {
                filtered.push_back(field);
            }
        }
        visible = std::move(filtered);
    }

    if (visible.empty()) {
        summary_->setText(fields_.empty() ? tr("這份文件沒有表單欄位")
                                          : tr("沒有符合條件的欄位"));
        return;
    }
    summary_->setText(tr("共 %1 個欄位").arg(visible.size()));

    for (const FieldTreeNode& node : alioth::engine::formbuild::buildFieldTree(visible)) {
        addNode(node, nullptr);
    }
    tree_->expandAll();
}

void FieldsPanel::addNode(const FieldTreeNode& node, QTreeWidgetItem* parent) {
    auto* item = parent == nullptr ? new QTreeWidgetItem(tree_) : new QTreeWidgetItem(parent);
    item->setText(0, QString::fromStdString(node.segment));
    item->setData(0, kNameRole, QString::fromStdString(node.fullName));
    item->setData(0, kIsFieldRole, node.isField);

    if (node.isField) {
        QString type = typeLabel(node.type);
        // 唯讀與必填用文字而不是圖示：這兩個屬性決定使用者要不要在這欄花時間，
        // 靠圖示傳達等於要求他先學會圖例。
        if (node.readOnly) type += tr("・唯讀");
        if (node.required) type += tr("・必填");
        if (node.widgetCount > 1) type += tr("・%1 個控制項").arg(node.widgetCount);
        item->setText(1, type);
        item->setText(2, node.pageIndex >= 0 ? QString::number(node.pageIndex + 1)
                                             : tr("未掛頁"));
        item->setData(0, kPageRole, node.pageIndex);
    } else {
        // 中介節點沒有實體欄位。標示出來，否則使用者會對著它按屬性。
        item->setText(1, tr("群組（%1 個欄位）").arg(node.fieldCount()));
        item->setDisabled(false);
        QFont font = item->font(0);
        font.setItalic(true);
        item->setFont(0, font);
    }

    for (const FieldTreeNode& child : node.children) addNode(child, item);
}

}  // namespace alioth::ui
