#include "ui/ribbon/ribbon_button.h"

#include <QEvent>
#include <QFontMetrics>

#include <algorithm>
#include <cmath>

#include "app/touch/touch_gestures.h"

namespace alioth::ui::ribbon {
namespace {

// 尺寸全部由字型高度推導，不寫死像素。理由有二：高 DPI 下 Qt 會連同字型一起放大，
// 跟著字型走就不會出現「圖示變大、按鈕沒變大」的錯位；使用者調大系統字級時同理。
constexpr double kLargeIconFactor = 2.0;
constexpr double kSmallIconFactor = 1.0;
constexpr double kLargeMinWidthFactor = 4.0;

}  // namespace

RibbonButton::RibbonButton(const Item& item, QWidget* parent)
    : QToolButton(parent), actionId_(item.actionId), size_(item.size) {
    setFocusPolicy(Qt::TabFocus);
    setAutoRaise(true);
    setObjectName(item.actionId);
    if (!item.label.isEmpty()) setText(item.label);
    applyMetrics();
}

void RibbonButton::applyMetrics() {
    const QFontMetrics metrics(font());
    const int unit = std::max(1, metrics.height());

    // 沒有圖示時退回純文字。
    //
    // TextUnderIcon / TextBesideIcon 即使圖示是空的，Qt 仍然依 iconSize 保留位置，
    // 結果是每顆按鈕的文字上方（或左方）掛著一塊空白，而空白的大小又隨按鈕
    // 大小不同——整排按鈕看起來就是高低參差、對不齊。症狀看似「版面亂」，
    // 原因其實是「圖示還沒補」，兩者從畫面上分不出來。
    if (icon().isNull()) {
        setToolButtonStyle(Qt::ToolButtonTextOnly);
    } else if (size_ == ItemSize::Large) {
        setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        setIconSize(QSize(static_cast<int>(unit * kLargeIconFactor),
                          static_cast<int>(unit * kLargeIconFactor)));
    } else {
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setIconSize(QSize(static_cast<int>(unit * kSmallIconFactor),
                          static_cast<int>(unit * kSmallIconFactor)));
    }
    updateGeometry();
}

void RibbonButton::setIcon(const QIcon& icon) {
    QToolButton::setIcon(icon);
    // 圖示是在建構之後才綁上來的（rebind），樣式必須跟著重算，
    // 否則先建好的按鈕會永遠停在「當初沒有圖示」的純文字樣式。
    applyMetrics();
}

void RibbonButton::setPlaceholder(bool placeholder) {
    placeholder_ = placeholder;
    setEnabled(!placeholder);
    if (placeholder) {
        setToolTip(tr("尚未提供的功能：%1").arg(actionId_));
    }
}

void RibbonButton::setTouchMode(bool enabled) {
    if (touchMode_ == enabled) return;
    touchMode_ = enabled;
    updateGeometry();
}

QSize RibbonButton::sizeHint() const {
    QSize hint = QToolButton::sizeHint();
    if (touchMode_) {
        // 44 CSS px 等效（PRD-UI-013）。用 logicalDpiX 換算而不是寫死像素：
        // 高 DPI 下寫死 44 裝置像素在實體上只有一半大，而觸控目標的門檻講的
        // 是手指按得到的實體尺寸。無條件進位，差半個像素也不能低於門檻。
        const double dpi = logicalDpiX() > 0 ? static_cast<double>(logicalDpiX())
                                             : app::TouchTargetMetrics::kReferenceDpi;
        const int minimum =
            static_cast<int>(std::ceil(app::TouchTargetMetrics::minimumTargetSizePx(dpi)));
        hint.setWidth(std::max(hint.width(), minimum));
        hint.setHeight(std::max(hint.height(), minimum));
    }
    if (size_ == ItemSize::Large) {
        // 大按鈕排成一列時寬度參差會讓整個群組看起來歪掉，給一個以字高為單位的下限。
        const QFontMetrics metrics(font());
        hint.setWidth(std::max(hint.width(), static_cast<int>(metrics.height() *
                                                              kLargeMinWidthFactor)));
    }
    return hint;
}

void RibbonButton::changeEvent(QEvent* event) {
    QToolButton::changeEvent(event);
    // 主題或 DPI 變更會先送 FontChange / StyleChange，這裡重算才不會維持舊尺寸。
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        applyMetrics();
    }
}

}  // namespace alioth::ui::ribbon
