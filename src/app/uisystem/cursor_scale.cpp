#include "app/uisystem/cursor_scale.h"

#include <QObject>
#include <QPainter>
#include <QPen>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace alioth::app {

double CursorScale::scaleFactor(CursorSizeLevel level) {
    switch (level) {
        case CursorSizeLevel::Normal: return 1.0;
        case CursorSizeLevel::Large: return 1.5;
        case CursorSizeLevel::ExtraLarge: return 2.0;
        case CursorSizeLevel::Huge: return 3.0;
    }
    return 1.0;
}

CursorSizeLevel CursorScale::fromSettingsValue(int value) {
    switch (value) {
        case 0: return CursorSizeLevel::Normal;
        case 1: return CursorSizeLevel::Large;
        case 2: return CursorSizeLevel::ExtraLarge;
        case 3: return CursorSizeLevel::Huge;
        default: return CursorSizeLevel::Normal;
    }
}

int CursorScale::toSettingsValue(CursorSizeLevel level) {
    switch (level) {
        case CursorSizeLevel::Normal: return 0;
        case CursorSizeLevel::Large: return 1;
        case CursorSizeLevel::ExtraLarge: return 2;
        case CursorSizeLevel::Huge: return 3;
    }
    return 0;
}

QString CursorScale::displayName(CursorSizeLevel level) {
    switch (level) {
        case CursorSizeLevel::Normal: return QObject::tr("標準");
        case CursorSizeLevel::Large: return QObject::tr("大");
        case CursorSizeLevel::ExtraLarge: return QObject::tr("特大");
        case CursorSizeLevel::Huge: return QObject::tr("超大");
    }
    return QObject::tr("標準");
}

QCursor CursorScale::buildCrosshairCursor(CursorSizeLevel level, qreal devicePixelRatio) {
    if (devicePixelRatio <= 0.0) devicePixelRatio = 1.0;
    const double logicalSize = kBaseSizePx * scaleFactor(level);
    const int physicalSize =
        std::max(1, static_cast<int>(std::lround(logicalSize * devicePixelRatio)));

    QPixmap pixmap(physicalSize, physicalSize);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(devicePixelRatio);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 用邏輯像素座標畫，devicePixelRatio 已經設在 pixmap 上，QPainter 會自動換算，
    // 不需要在這裡再乘一次——重複乘會讓 HiDPI 螢幕上的準星比預期大一倍。
    const double half = logicalSize / 2.0;
    const double gapRatio = 0.18;  // 十字中央留白，避免熱點正中央被線條蓋住看不清楚
    const double gap = logicalSize * gapRatio;

    // 白色描邊 + 黑色主線，確保在深色與淺色背景下都看得見（PRD-A11Y-005 的
    // 「焦點可見」精神同樣適用於自訂游標）。
    QPen outline(Qt::white, 3.0);
    QPen main(Qt::black, 1.5);

    const auto drawCross = [&](const QPen& pen) {
        painter.setPen(pen);
        painter.drawLine(QPointF(half, 0.0), QPointF(half, half - gap));
        painter.drawLine(QPointF(half, half + gap), QPointF(half, logicalSize));
        painter.drawLine(QPointF(0.0, half), QPointF(half - gap, half));
        painter.drawLine(QPointF(half + gap, half), QPointF(logicalSize, half));
    };
    drawCross(outline);
    drawCross(main);
    painter.end();

    // 熱點必須是「邏輯像素座標」，QCursor 的 hotspot 參數不吃 devicePixelRatio
    // 的縮放——用 physicalSize 除以 devicePixelRatio 換算回邏輯座標，
    // 而不是直接用 half（那是用 kBaseSizePx*scaleFactor 算出來的，
    // 與 physicalSize/devicePixelRatio 理論上相等，但用同一份換算避免累積誤差）。
    const int hotspot = static_cast<int>(std::lround(physicalSize / devicePixelRatio / 2.0));
    return QCursor(pixmap, hotspot, hotspot);
}

}  // namespace alioth::app
