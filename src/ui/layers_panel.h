#pragma once

// 圖層（OCG）面板（PRD-VIEW-008）。薄外殼：所有互斥群組/鎖定邏輯都在
// domain::setLayerVisible（見 domain/ocg.h），這裡只做「畫出樹狀勾選框、
// 把使用者點擊換成呼叫那個函式」。
//
// 已知邊界：勾選目前只更新本面板與 DocumentController::layers() 的模型，
// *不會*觸發重新渲染——PDFium 的公開 API 沒有執行期 OC 狀態入口
// （見 domain/ocg.h 開頭與 exceptions/EXC_20260906_RD_SA_ocg_render_gap.md）。
// 面板固定顯示一行提示，不讓使用者誤以為勾選會改變畫面。

#include <QWidget>

#include "domain/ocg.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

namespace alioth::ui {

class LayersPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayersPanel(QWidget* parent = nullptr);

    // 呼叫端（通常是 DocumentController::layersReady 的處理器）在拿到新樹時呼叫。
    void setTree(const domain::OcgTree& tree);
    [[nodiscard]] const domain::OcgTree& tree() const noexcept { return tree_; }

signals:
    // 使用者勾選/取消勾選了某個圖層。呼叫端應該同時呼叫
    // DocumentController::setLayerVisible 讓兩邊模型一致，並依上面的已知邊界
    // 決定要不要另外提示使用者「畫面不會變」。
    void layerVisibilityRequested(int objectNumber, bool visible);

private:
    void rebuildTree();
    void addNode(std::int32_t index, QTreeWidgetItem* parent);
    void handleItemChanged(QTreeWidgetItem* item, int column);

    domain::OcgTree tree_;
    QTreeWidget* view_{nullptr};
    QLabel* notice_{nullptr};
    bool applyingModel_{false};  // 重建樹時避免 itemChanged 誤觸再次呼叫模型
};

}  // namespace alioth::ui
