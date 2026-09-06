#include "ui/attachments_panel.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "engine/attachments/attachment_reader.h"

namespace alioth::ui {
namespace {

using alioth::engine::attachments::Attachment;
using alioth::engine::attachments::AttachmentOrigin;

constexpr int kIndexRole = Qt::UserRole + 1;

QString sizeText(const Attachment& attachment) {
    if (attachment.size < 0) return QCoreApplication::translate("AttachmentsPanel", "大小不明");
    const QString formatted = QLocale().formattedDataSize(attachment.size);
    if (!attachment.sizeMismatch) return formatted;
    // 宣告的大小與實際取得的不符。不標示的話，使用者只會覺得存出來的檔案壞了。
    return QCoreApplication::translate("AttachmentsPanel", "%1（與實際內容不符）").arg(formatted);
}

}  // namespace

AttachmentsPanel::AttachmentsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("attachmentsPanel"));
    setAccessibleName(tr("附件"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    summary_ = new QLabel(tr("這份文件沒有附件"), this);
    summary_->setObjectName(QStringLiteral("attachmentsSummary"));
    summary_->setAccessibleName(tr("附件摘要"));
    summary_->setWordWrap(true);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("attachmentsTree"));
    tree_->setAccessibleName(tr("附件清單"));
    tree_->setAccessibleDescription(tr("Enter 為另存新檔，不是開啟附件"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("檔名"), tr("大小"), tr("來源")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setRootIsDecorated(false);

    save_ = new QPushButton(tr("另存新檔…"), this);
    save_->setObjectName(QStringLiteral("attachmentsSave"));
    save_->setEnabled(false);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(save_);
    buttons->addStretch();

    layout->addWidget(summary_);
    layout->addWidget(tree_);
    layout->addLayout(buttons);

    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                save_->setEnabled(current != nullptr);
            });

    const auto requestSave = [this] {
        QTreeWidgetItem* item = tree_->currentItem();
        if (item == nullptr) return;
        const int index = item->data(0, kIndexRole).toInt();
        if (index < 0 || index >= static_cast<int>(attachments_.size())) return;
        const std::string safe =
            alioth::engine::attachments::sanitizeAttachmentFileName(attachments_[
                static_cast<std::size_t>(index)].fileName);
        emit saveRequested(index, QString::fromStdString(safe));
    };
    connect(save_, &QPushButton::clicked, this, requestSave);
    // 雙擊是另存，不是開啟。雙擊就執行附件正是 PDF 被當成惡意程式載體的主要途徑。
    connect(tree_, &QTreeWidget::itemActivated, this, requestSave);
}

AttachmentsPanel::~AttachmentsPanel() = default;

void AttachmentsPanel::setAttachments(std::vector<Attachment> attachments) {
    attachments_ = std::move(attachments);
    rebuild();
}

void AttachmentsPanel::rebuild() {
    tree_->clear();
    save_->setEnabled(false);

    if (attachments_.empty()) {
        summary_->setText(tr("這份文件沒有附件"));
        return;
    }
    summary_->setText(tr("共 %1 個附件。附件不會被開啟或執行，只能另存到您選擇的位置。")
                          .arg(attachments_.size()));

    for (std::size_t i = 0; i < attachments_.size(); ++i) {
        const Attachment& attachment = attachments_[i];
        auto* item = new QTreeWidgetItem(tree_);
        const QString name = attachment.fileName.empty()
                                 ? tr("（未命名）")
                                 : QString::fromStdString(attachment.fileName);
        item->setText(0, name);
        item->setText(1, sizeText(attachment));
        item->setText(2, attachment.origin == AttachmentOrigin::DocumentLevel
                             ? tr("文件層")
                             : tr("第 %1 頁的註解").arg(attachment.pageIndex + 1));
        if (!attachment.description.empty()) {
            item->setToolTip(0, QString::fromStdString(attachment.description));
        }
        item->setData(0, kIndexRole, static_cast<int>(i));
    }
}

}  // namespace alioth::ui
