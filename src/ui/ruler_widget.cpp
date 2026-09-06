#include "ui/ruler_widget.h"

#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace alioth::ui {

namespace {
constexpr int kRulerThickness = 20;
}  // namespace

RulerWidget::RulerWidget(Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent), orientation_(orientation) {
    setMouseTracking(true);
    setObjectName(orientation == Qt::Horizontal ? QStringLiteral("rulerHorizontal")
                                                : QStringLiteral("rulerVertical"));
    setAccessibleName(orientation == Qt::Horizontal ? tr("水平尺規") : tr("垂直尺規"));
    if (orientation_ == Qt::Horizontal) {
        setFixedHeight(kRulerThickness);
    } else {
        setFixedWidth(kRulerThickness);
    }
}

QSize RulerWidget::sizeHint() const {
    return orientation_ == Qt::Horizontal ? QSize(200, kRulerThickness)
                                          : QSize(kRulerThickness, 200);
}

void RulerWidget::setMapping(const domain::PageTransform& transform, double originPx) {
    transform_ = transform;
    originPx_ = originPx;
    update();
}

double RulerWidget::widgetToPagePosition(int widgetCoordinate) const {
    const double devicePx = widgetCoordinate - originPx_;
    // 只關心單一軸：另一軸給 0，PageTransform::toPage 兩軸各自獨立換算，
    // 不會互相汙染（見 geometry.h 的仿射轉換）。
    const domain::PointF devicePoint = orientation_ == Qt::Horizontal
                                           ? domain::PointF{devicePx, 0.0}
                                           : domain::PointF{0.0, devicePx};
    const domain::PointF pagePoint = transform_.toPage(devicePoint);
    return orientation_ == Qt::Horizontal ? pagePoint.x : pagePoint.y;
}

void RulerWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    painter.setPen(palette().mid().color());

    // 每 50pt(約 1.76cm)一個主刻度，附帶頁面座標數字；純畫刻度，
    // 不做貼齊判斷——貼齊只在使用者實際拖曳時才需要（domain::snapToGrid）。
    constexpr double kMajorStepPt = 50.0;
    const int length = orientation_ == Qt::Horizontal ? width() : height();
    for (int pixel = 0; pixel < length; ++pixel) {
        const double pagePt = widgetToPagePosition(pixel);
        if (std::abs(std::fmod(pagePt, kMajorStepPt)) > 1.0) continue;
        if (orientation_ == Qt::Horizontal) {
            painter.drawLine(pixel, kRulerThickness / 2, pixel, kRulerThickness);
        } else {
            painter.drawLine(kRulerThickness / 2, pixel, kRulerThickness, pixel);
        }
    }
}

void RulerWidget::mousePressEvent(QMouseEvent* event) {
    dragging_ = true;
    const int coordinate = orientation_ == Qt::Horizontal ? event->pos().x() : event->pos().y();
    emit guideDragged(widgetToPagePosition(coordinate));
}

void RulerWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    const int coordinate = orientation_ == Qt::Horizontal ? event->pos().x() : event->pos().y();
    emit guideDragged(widgetToPagePosition(coordinate));
}

void RulerWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_) return;
    dragging_ = false;
    const int coordinate = orientation_ == Qt::Horizontal ? event->pos().x() : event->pos().y();
    // 放開時若還在尺規本身範圍內（沒有真的拖進頁面），視為取消——單純點擊
    // 尺規不該建立一條位置隨機的參考線。
    if (rect().contains(event->pos())) {
        emit guideDragCanceled();
        return;
    }
    emit guideCommitted(widgetToPagePosition(coordinate));
}

}  // namespace alioth::ui
