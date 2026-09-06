#include "app/touch/touch_gestures.h"

#include <algorithm>
#include <cmath>

namespace alioth::app {

LongPressGesture::LongPressGesture(Config config) : config_(config) {}

void LongPressGesture::press(const QPointF& position, std::int64_t timestampMs) {
    tracking_ = true;
    eligible_ = true;
    fired_ = false;
    startPosition_ = position;
    startTimestampMs_ = timestampMs;
}

void LongPressGesture::move(const QPointF& position, std::int64_t /*timestampMs*/) {
    if (!tracking_ || !eligible_) return;
    const double dx = position.x() - startPosition_.x();
    const double dy = position.y() - startPosition_.y();
    const double distance = std::hypot(dx, dy);
    // 超過門檻就永久失去資格：這是「位移超過門檻就不算長按而是拖曳」的實作點。
    // 刻意不比較「回到原點就恢復資格」——使用者手指一旦滑出容許範圍，
    // 意圖已經是拖曳，之後晃回起點不代表他改變心意了。
    if (distance > config_.moveToleranceDevicePx) {
        eligible_ = false;
    }
}

void LongPressGesture::release() {
    tracking_ = false;
    eligible_ = false;
}

bool LongPressGesture::checkTimeout(std::int64_t nowMs) {
    if (!tracking_ || !eligible_ || fired_) return false;
    if (nowMs - startTimestampMs_ < config_.pressDurationMs) return false;
    fired_ = true;
    return true;
}

void PinchZoomTracker::begin(const QPointF& point1, const QPointF& point2, double startScale) {
    const double dx = point2.x() - point1.x();
    const double dy = point2.y() - point1.y();
    // 起始距離避免為零：兩指幾乎重疊時距離趨近 0，任何後續距離除以它都會
    // 產生失控的倍率跳變。夾一個極小值下限，讓那個畸形手勢的效果是
    // 「倍率變化很大但有限」，而不是無限大或除以零的未定義行為。
    startDistance_ = std::max(std::hypot(dx, dy), 1.0);
    startScale_ = startScale;
}

PinchZoomTracker::Update PinchZoomTracker::update(const QPointF& point1,
                                                  const QPointF& point2) const {
    const double dx = point2.x() - point1.x();
    const double dy = point2.y() - point1.y();
    const double currentDistance = std::hypot(dx, dy);
    Update result;
    result.scale = startScale_ * (currentDistance / startDistance_);
    result.anchor = QPointF((point1.x() + point2.x()) / 2.0, (point1.y() + point2.y()) / 2.0);
    return result;
}

}  // namespace alioth::app
