#include "ui/pan_zoom_panel.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QAccessibleEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>

#include <algorithm>

namespace alioth::ui {
namespace {

// 給 PanZoomPanel 一個有意義的無障礙角色。
//
// 為什麼需要這一段：QWidget 的預設角色是 Client，而 Client 對輔助技術而言
// 等於「一塊不知道是什麼的區域」——使用者聽到的是面板名稱加上「用戶端」，
// 完全推不出這是可以用方向鍵移動的導覽控制項。設 accessibleName 不會改角色，
// 角色只能透過 QAccessibleInterface 提供，因此必須裝一個工廠。
//
// 選 Canvas 而不是 Slider：它是二維的（同時控制水平與垂直位置），
// 而 Slider 在輔助技術裡意味著單一維度的數值，會誤導使用者去找「值是多少」。
class PanZoomAccessible : public QAccessibleWidget {
public:
    explicit PanZoomAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Canvas) {}
};

QAccessibleInterface* panZoomAccessibleFactory(const QString& className, QObject* object) {
    if (className == QLatin1String("alioth::ui::PanZoomPanel")) {
        if (auto* widget = qobject_cast<QWidget*>(object)) return new PanZoomAccessible(widget);
    }
    return nullptr;
}

}  // namespace

PanZoomPanel::PanZoomPanel(app::DocumentController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setFixedSize(200, 260);

    // 這個面板是可操作控制項，不是裝飾。沒有 StrongFocus 的話 Tab 到不了，
    // 而 Tab 到不了就等於鍵盤使用者沒有這個功能。
    // 工廠只需要裝一次。static 區域變數的初始化是執行緒安全的，
    // 而且保證在第一個面板建立時才發生（不是在程式啟動時）。
    static const bool accessibleFactoryInstalled = [] {
        QAccessible::installFactory(&panZoomAccessibleFactory);
        return true;
    }();
    Q_UNUSED(accessibleFactoryInstalled);

    setObjectName(QStringLiteral("panZoomPanel"));
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("平移與縮放導覽"));
    setAccessibleDescription(
        tr("方向鍵移動可視範圍，PageUp 與 PageDown 移動一整個畫面，Home 回到左上角"));

    connect(controller_, &app::DocumentController::thumbnailReady, this,
            [this](int pageIndex, const QImage& image) {
                if (pageIndex != pageIndex_) return;
                thumbnail_ = image;
                const domain::SizeF pagePt = controller_->pageSizePt(pageIndex_);
                thumbnailScale_ = domain::panZoomThumbnailScale(
                    pagePt.isEmpty() ? domain::SizeF{double(image.width()), double(image.height())}
                                     : pagePt,
                    domain::SizeF{static_cast<double>(width()), static_cast<double>(height())});
                update();
            });
}

void PanZoomPanel::setPage(std::int32_t pageIndex) {
    if (pageIndex_ == pageIndex) return;
    pageIndex_ = pageIndex;
    thumbnail_ = QImage();
    if (pageIndex_ >= 0) {
        controller_->requestThumbnail(pageIndex_,
                                      std::max(width(), height()));
    }
}

void PanZoomPanel::setMainViewport(const domain::RectI& viewportInPageDevice, double mainScale) {
    mainViewport_ = viewportInPageDevice;
    mainScale_ = mainScale;
    update();
}

void PanZoomPanel::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    // Base/Text 是 Qt 保證互相對比的角色配對，高對比佈景下系統會一起換掉。
    // 原本的 dark() 底 + 硬編白字在高對比模式下會變成白底白字。
    painter.fillRect(rect(), palette().base());
    if (!thumbnail_.isNull()) {
        painter.drawImage(rect(), thumbnail_);
    } else {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect(), Qt::AlignCenter, tr("縮圖載入中"));
    }

    if (mainScale_ <= 0.0 || thumbnailScale_ <= 0.0) return;
    const domain::ThumbnailRectF box =
        domain::mainViewportToThumbnailRect(mainViewport_, mainScale_, thumbnailScale_);
    // 「目前可視範圍」框改用 Highlight。紅色在高對比佈景下不一定與底色有對比，
    // 而 Highlight 正是系統定義的「目前作用中」顏色，會隨佈景一起換。
    // 這個框的意義由位置與形狀承載，不靠顏色，因此換色不影響可辨識性。
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(box.x, box.y, box.width, box.height));
}

void PanZoomPanel::mousePressEvent(QMouseEvent* event) {
    dragging_ = true;
    setFocus(Qt::MouseFocusReason);
    dragTo(event->position());
}

void PanZoomPanel::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    dragTo(event->position());
}

QPointF PanZoomPanel::viewportCentreInWidget() const {
    const domain::ThumbnailRectF box =
        domain::mainViewportToThumbnailRect(mainViewport_, mainScale_, thumbnailScale_);
    return QPointF(box.x + box.width / 2.0, box.y + box.height / 2.0);
}

void PanZoomPanel::nudge(double dx, double dy) {
    if (pageIndex_ < 0 || thumbnailScale_ <= 0.0 || mainScale_ <= 0.0) return;
    const QPointF centre = viewportCentreInWidget();
    // 走與滑鼠拖曳相同的 dragTo，而不是自己再算一次座標換算。
    // 兩條路徑各算一次的話，鍵盤與滑鼠會在邊界情況下產生不同結果，
    // 而那種差異只有鍵盤使用者會遇到，因此最不容易被發現。
    dragTo(centre + QPointF(dx, dy));
    updateAccessibleState();
}

void PanZoomPanel::keyPressEvent(QKeyEvent* event) {
    const domain::ThumbnailRectF box =
        domain::mainViewportToThumbnailRect(mainViewport_, mainScale_, thumbnailScale_);
    // 小步以視框的十分之一為單位：固定像素數在不同縮圖比例下的手感差距很大，
    // 縮小到 8% 時一格會變成幾乎不動。
    const double stepX = std::max(1.0, box.width / 10.0);
    const double stepY = std::max(1.0, box.height / 10.0);

    switch (event->key()) {
        case Qt::Key_Left:     nudge(-stepX, 0.0); break;
        case Qt::Key_Right:    nudge(stepX, 0.0); break;
        case Qt::Key_Up:       nudge(0.0, -stepY); break;
        case Qt::Key_Down:     nudge(0.0, stepY); break;
        case Qt::Key_PageUp:   nudge(0.0, -box.height); break;
        case Qt::Key_PageDown: nudge(0.0, box.height); break;
        case Qt::Key_Home:
            if (pageIndex_ >= 0 && thumbnailScale_ > 0.0) {
                dragTo(QPointF(box.width / 2.0, box.height / 2.0));
                updateAccessibleState();
            }
            break;
        default:
            QWidget::keyPressEvent(event);
            return;
    }
    event->accept();
}

void PanZoomPanel::updateAccessibleState() {
    if (pageIndex_ < 0) return;
    const QString status = tr("第 %1 頁，可視範圍位於 %2, %3")
                               .arg(pageIndex_ + 1)
                               .arg(mainViewport_.x)
                               .arg(mainViewport_.y);
    if (accessibleDescription() == status) return;
    setAccessibleDescription(status);
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}

void PanZoomPanel::dragTo(const QPointF& widgetPoint) {
    if (pageIndex_ < 0 || thumbnailScale_ <= 0.0) return;
    // 讓視框中心對齊點擊位置，而不是把點擊位置當視框左上角——
    // 那樣使用者點哪裡就會覺得畫面該跳到哪裡，而不是跳到「點擊點右下方一塊」。
    const domain::ThumbnailRectF centered{
        widgetPoint.x() - mainViewport_.width * thumbnailScale_ / mainScale_ / 2.0,
        widgetPoint.y() - mainViewport_.height * thumbnailScale_ / mainScale_ / 2.0,
        mainViewport_.width * thumbnailScale_ / mainScale_,
        mainViewport_.height * thumbnailScale_ / mainScale_,
    };
    const domain::PointF origin =
        domain::thumbnailRectToMainOrigin(centered, mainScale_, thumbnailScale_);
    emit viewportOriginRequested(pageIndex_, origin);
}

}  // namespace alioth::ui
