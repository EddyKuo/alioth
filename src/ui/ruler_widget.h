#pragma once

// 尺規（PRD-VIEW-015）：沿檢視上緣／左緣顯示刻度，拖曳可以拉出參考線。
//
// 座標轉換一律走 domain::PageTransform（見 geometry.h），不在這裡自行翻轉或
// 縮放——那正是 CLAUDE.md 反覆強調、也是本檔案存在唯一該小心的地方。
// 貼齊判斷本身在 domain/guides.h，已有獨立單元測試涵蓋往返自洽；這個檔案
// 只負責畫刻度與把滑鼠事件換成 domain 呼叫的薄外殼。

#include <QWidget>

#include "domain/geometry.h"
#include "domain/guides.h"

namespace alioth::ui {

class RulerWidget : public QWidget {
    Q_OBJECT

public:
    RulerWidget(Qt::Orientation orientation, QWidget* parent = nullptr);

    // 呈現層（PageView）每次版面或捲動改變時呼叫，讓尺規知道怎麼把自己的
    // 座標換成頁面座標。origin 是目前頁面左上角在尺規座標系中的位置（像素），
    // 已經扣掉捲動位移。
    void setMapping(const domain::PageTransform& transform, double originPx);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

signals:
    // 使用者從尺規拖出一條參考線，position 是預覽中的即時頁面座標
    // （水平尺規給 x，垂直尺規給 y）。
    void guideDragged(double positionPt);
    // 放開滑鼠：這條參考線正式成立。
    void guideCommitted(double positionPt);
    void guideDragCanceled();

private:
    [[nodiscard]] double widgetToPagePosition(int widgetCoordinate) const;

    Qt::Orientation orientation_;
    domain::PageTransform transform_;
    double originPx_{0.0};
    bool dragging_{false};
};

}  // namespace alioth::ui
