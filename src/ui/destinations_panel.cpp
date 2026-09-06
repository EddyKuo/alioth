#include "ui/destinations_panel.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/navigation_service.h"

namespace alioth::ui {
namespace {

constexpr int kPageRole = Qt::UserRole + 1;
constexpr int kInheritRole = Qt::UserRole + 2;

}  // namespace

DestinationsPanel::DestinationsPanel(alioth::app::NavigationService* service, QWidget* parent)
    : QWidget(parent), service_(service) {
    setObjectName(QStringLiteral("destinationsPanel"));
    setAccessibleName(tr("命名目標"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("destinationsSearch"));
    search_->setAccessibleName(tr("搜尋命名目標"));
    search_->setPlaceholderText(tr("搜尋目標名稱"));
    search_->setClearButtonEnabled(true);

    status_ = new QLabel(tr("尚未開啟文件"), this);
    status_->setObjectName(QStringLiteral("destinationsStatus"));
    status_->setAccessibleName(tr("命名目標狀態"));
    status_->setWordWrap(true);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("destinationsTree"));
    tree_->setAccessibleName(tr("命名目標清單"));
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("名稱"), tr("位置")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setRootIsDecorated(false);

    layout->addWidget(search_);
    layout->addWidget(status_);
    layout->addWidget(tree_);

    connect(search_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr || item->isDisabled()) return;
        emit destinationActivated(item->data(0, kPageRole).toInt(),
                                  item->data(0, kInheritRole).toBool());
    });
}

void DestinationsPanel::setDocument(const QString& path) {
    if (service_ == nullptr) return;
    if (path.isEmpty()) {
        service_->clear();
        status_->setText(tr("尚未開啟文件"));
        rebuild();
        return;
    }

    std::string diagnostic;
    if (!service_->load(path, diagnostic)) {
        // 讀不到與「沒有命名目標」是兩件事，面板必須分得出來，
        // 否則使用者會以為文件裡沒有條號錨點而去別處找。
        status_->setText(tr("讀取失敗：%1").arg(QString::fromStdString(diagnostic)));
        rebuild();
        return;
    }
    status_->setText(service_->rows().empty() ? tr("這份文件沒有命名目標")
                                             : tr("共 %1 個目標").arg(service_->rows().size()));
    rebuild();
}

void DestinationsPanel::rebuild() {
    tree_->clear();
    if (service_ == nullptr) return;

    for (const alioth::app::DestinationRow& row : service_->filter(search_->text())) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, row.name);
        item->setText(1, row.describe());
        item->setData(0, kPageRole, row.pageIndex);
        item->setData(0, kInheritRole, row.inheritsZoom());
        if (row.broken) item->setDisabled(true);
    }
}

}  // namespace alioth::ui
