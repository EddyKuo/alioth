#include "ui/loupe_widget.h"

#include <QPainter>
#include <QPalette>
#include <QPen>

#include <algorithm>

#include "domain/tile.h"

namespace alioth::ui {

LoupeWidget::LoupeWidget(app::DocumentController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setFixedSize(180, 180);
    // 放大鏡是純顯示元件：內容由主視圖的游標位置驅動，本身沒有可操作的東西。
    // 因此不給焦點是刻意的，但仍需要名稱——否則螢幕閱讀器使用者掃過面板時
    // 會遇到一塊無名的空白。
    setObjectName(QStringLiteral("loupeWidget"));
    setAccessibleName(tr("放大鏡"));
    setAccessibleDescription(tr("顯示主視圖游標所在位置的放大影像"));
    connect(controller_, &app::DocumentController::tileReady, this,
            [this](domain::TileKey) { update(); });
}

void LoupeWidget::setMagnification(double magnification) {
    magnification_ = std::clamp(magnification, 1.5, 8.0);
}

void LoupeWidget::updateCursor(std::int32_t pageIndex, const domain::PointF& cursorAtBaseScale,
                               double baseScale) {
    pageIndex_ = pageIndex;

    domain::LoupeRequest request;
    request.cursorAtBaseScale = cursorAtBaseScale;
    request.baseScale = baseScale;
    request.magnification = magnification_;
    request.loupeWidgetPx = domain::SizeF{static_cast<double>(width()), static_cast<double>(height())};
    sample_ = domain::computeLoupeSample(request);

    if (pageIndex_ < 0 || sample_.sampleRect.isEmpty()) {
        update();
        return;
    }

    const domain::SizeF pagePt = controller_->pageSizePt(pageIndex_);
    const domain::RectI pageSizeAtEffectiveScale{
        0, 0, static_cast<std::int32_t>(std::lround(pagePt.width * sample_.effectiveScale)),
        static_cast<std::int32_t>(std::lround(pagePt.height * sample_.effectiveScale))};

    app::PageTileRequest tileRequest;
    tileRequest.pageIndex = pageIndex_;
    tileRequest.visibleInPage = sample_.sampleRect;
    tileRequest.pageSize = pageSizeAtEffectiveScale;

    app::TileScheduleOptions options;
    options.scale = sample_.effectiveScale;
    options.rotation = domain::Rotation::None;
    // 放大鏡的可視區只有一小塊，且是使用者正在盯著看的地方——不給它預取，
    // 也不走「縮放中」的粗略倍率路徑，直接要精確倍率的圖磚。
    options.zooming = false;
    controller_->scheduleTiles({tileRequest}, options);

    update();
}

void LoupeWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    // 底色與提示文字一律取自 Base/Text 這一組角色（PRD-A11Y-005）。
    // 它們是 Qt 保證互相對比的配對，使用者切到 Windows 高對比佈景時
    // 系統會把高對比色塞進同一組角色，硬編黑白則會整塊蓋掉系統設定。
    painter.fillRect(rect(), palette().base());

    if (pageIndex_ < 0 || sample_.sampleRect.isEmpty()) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect(), Qt::AlignCenter, tr("移到頁面上以放大"));
        return;
    }

    const domain::RectI& visible = sample_.sampleRect;
    const std::int32_t firstCol = std::max(0, visible.x) / domain::kTileSize;
    const std::int32_t firstRow = std::max(0, visible.y) / domain::kTileSize;
    const std::int32_t lastCol = (visible.right() - 1) / domain::kTileSize;
    const std::int32_t lastRow = (visible.bottom() - 1) / domain::kTileSize;
    const std::int32_t scaleKey = domain::exactScaleKey(sample_.effectiveScale);

    for (std::int32_t row = firstRow; row <= lastRow; ++row) {
        for (std::int32_t col = firstCol; col <= lastCol; ++col) {
            const domain::TileKey key{pageIndex_, scaleKey, col, row, domain::Rotation::None, false};
            const QImage tile = controller_->tileIfReady(key);
            if (tile.isNull()) continue;
            painter.drawImage(col * domain::kTileSize - visible.x, row * domain::kTileSize - visible.y,
                              tile);
        }
    }

    // 圓形觀景窗與外框：放大鏡的視覺慣例，也讓使用者清楚知道邊界外不是畫面。
    painter.setPen(QPen(palette().color(QPalette::Mid), 2));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));
}

}  // namespace alioth::ui
