#pragma once

// Loupe 放大鏡（PRD-ZOOM-004）。
//
// 嚴禁整頁光柵化（docs/SDD.md 架構限制 2）：這裡不是另開一條光柵化路徑，
// 而是把游標位置換算成一個小矩形（domain::computeLoupeSample，已有單元測試），
// 用既有的圖磚管線在「有效倍率」（baseScale × magnification）下渲染那一小塊，
// 與 PageView 用的是同一個 DocumentController::scheduleTiles /
// tileIfReady——差別只在請求的可視區很小、倍率通常比主視圖高。

#include <QWidget>

#include "app/document_controller.h"
#include "domain/geometry.h"
#include "domain/zoom_aids.h"

namespace alioth::ui {

class LoupeWidget : public QWidget {
    Q_OBJECT

public:
    LoupeWidget(app::DocumentController* controller, QWidget* parent = nullptr);

    void setMagnification(double magnification);
    [[nodiscard]] double magnification() const noexcept { return magnification_; }

    // pageIndex 與 cursorAtBaseScale 一律是「頁內裝置空間」座標（原點頁面左上、
    // Y 向下），與 PageView::hitTest 之後再轉一次不同——呼叫端已經知道游標在哪一頁。
    void updateCursor(std::int32_t pageIndex, const domain::PointF& cursorAtBaseScale,
                      double baseScale);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    app::DocumentController* controller_{nullptr};
    double magnification_{2.0};
    std::int32_t pageIndex_{-1};
    domain::LoupeSample sample_{};
};

}  // namespace alioth::ui
