#pragma once

// Pan & Zoom 面板（PRD-ZOOM-005）：縮圖 + 可拖曳的視框，代表主視圖目前看到
// 的範圍；拖曳視框改變主視圖的捲動位置。
//
// 縮圖渲染是架構文件明列的整頁光柵化例外（docs/SDD.md 架構限制 2），
// 走既有的 DocumentController::requestThumbnail，不另外開路徑。
// 視框與主視圖之間的座標換算在 domain::zoom_aids.h，已有往返測試涵蓋。

#include <QImage>
#include <QWidget>

#include "app/document_controller.h"
#include "domain/geometry.h"
#include "domain/zoom_aids.h"

namespace alioth::ui {

class PanZoomPanel : public QWidget {
    Q_OBJECT

public:
    explicit PanZoomPanel(app::DocumentController* controller, QWidget* parent = nullptr);

    void setPage(std::int32_t pageIndex);
    // 主視圖每次可視區變更時呼叫（可視區與倍率都是文件裝置空間，見
    // domain/zoom_aids.h 開頭說明）。
    void setMainViewport(const domain::RectI& viewportInPageDevice, double mainScale);

signals:
    // 使用者拖曳視框，換算出的主視圖新捲動原點（頁內裝置空間，主視圖倍率）。
    void viewportOriginRequested(int pageIndex, const alioth::domain::PointF& origin);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    // PRD-A11Y-005：這個面板原本只有滑鼠拖曳一條操作路徑，鍵盤使用者
    // 完全無法使用。方向鍵以視框大小的十分之一步進，PageUp/PageDown 以
    // 一整個視框高度移動，Home 回到左上角。
    void keyPressEvent(QKeyEvent* event) override;

private:
    void dragTo(const QPointF& widgetPoint);
    // 視框中心的目前位置（面板座標）。鍵盤移動以它為基準，
    // 才能與滑鼠拖曳共用同一條換算路徑而不是另寫一套。
    [[nodiscard]] QPointF viewportCentreInWidget() const;
    void nudge(double dx, double dy);
    void updateAccessibleState();

    app::DocumentController* controller_{nullptr};
    std::int32_t pageIndex_{-1};
    QImage thumbnail_;
    domain::RectI mainViewport_{};
    double mainScale_{1.0};
    double thumbnailScale_{1.0};
    bool dragging_{false};
};

}  // namespace alioth::ui
