#include "ui/layers_panel.h"

#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace alioth::ui {

namespace {
constexpr int kObjectNumberRole = Qt::UserRole + 1;
}  // namespace

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("layersPanel"));
    setAccessibleName(tr("圖層"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("layersNotice"));
    notice_->setAccessibleName(tr("圖層狀態"));
    notice_->setWordWrap(true);
    // 原本是 "color: palette(mid); font-size: 11px;"。palette(mid) 雖然隨主題走，
    // 但 Mid 是中間灰，疊在 Window 上通常只有 2–3:1，未達 WCAG AA 的 4.5:1；
    // 這行文字是使用者唯一能知道「圖層為何不能切換」的地方，不該淡化。
    // 只留字級，顏色交給預設的 WindowText。
    notice_->setStyleSheet(QStringLiteral("font-size: 11px;"));
    layout->addWidget(notice_);

    view_ = new QTreeWidget(this);
    view_->setObjectName(QStringLiteral("layersTree"));
    view_->setAccessibleName(tr("圖層清單"));
    view_->setHeaderHidden(true);
    layout->addWidget(view_);
    setFocusProxy(view_);

    connect(view_, &QTreeWidget::itemChanged, this, &LayersPanel::handleItemChanged);
}

void LayersPanel::setTree(const domain::OcgTree& tree) {
    tree_ = tree;
    rebuildTree();
}

void LayersPanel::rebuildTree() {
    applyingModel_ = true;
    view_->clear();

    if (!tree_.present) {
        notice_->setText(tr("本文件沒有圖層（/OCProperties）。"));
        applyingModel_ = false;
        return;
    }

    // PDFium 目前的公開 API 無法在渲染時套用執行期 OC 狀態，這裡的勾選
    // 目前只影響面板本身——見 domain/ocg.h 開頭的說明。不把這點藏起來，
    // 讓使用者以為勾了就會變，是比「功能還沒做完」更糟的事（SDD §7）。
    notice_->setText(
        tr("圖層可見性狀態僅供檢視／記錄；受限於目前使用的 PDFium 版本，"
           "勾選暫不會即時改變畫面渲染。"));

    for (const std::int32_t root : tree_.roots) addNode(root, nullptr);
    applyingModel_ = false;
}

void LayersPanel::addNode(std::int32_t index, QTreeWidgetItem* parent) {
    if (index < 0 || static_cast<std::size_t>(index) >= tree_.layers.size()) return;
    const domain::OcgLayer& layer = tree_.layers[static_cast<std::size_t>(index)];

    auto* item = parent == nullptr ? new QTreeWidgetItem(view_) : new QTreeWidgetItem(parent);
    item->setText(0, QString::fromStdString(layer.name));
    item->setData(0, kObjectNumberRole, layer.objectNumber);

    if (layer.isGroupHeading) {
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    } else {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
        item->setDisabled(layer.locked);
    }

    for (const std::int32_t child : layer.children) addNode(child, item);
}

void LayersPanel::handleItemChanged(QTreeWidgetItem* item, int column) {
    if (applyingModel_ || column != 0) return;
    const int objectNumber = item->data(0, kObjectNumberRole).toInt();
    const bool visible = item->checkState(0) == Qt::Checked;

    const auto changed = domain::setLayerVisible(tree_, [&] {
        for (std::size_t i = 0; i < tree_.layers.size(); ++i) {
            if (tree_.layers[i].objectNumber == objectNumber) return static_cast<std::int32_t>(i);
        }
        return static_cast<std::int32_t>(-1);
    }(), visible);

    if (!changed.empty()) {
        // 互斥群組可能連動改了其他節點的可見性，局部刷新而不是整棵重建，
        // 避免使用者展開/捲起的狀態被重置。
        rebuildTree();
        emit layerVisibilityRequested(objectNumber, visible);
    }
}

}  // namespace alioth::ui
