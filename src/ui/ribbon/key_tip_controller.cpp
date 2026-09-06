#include "ui/ribbon/key_tip_controller.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QWidget>

namespace alioth::ui::ribbon {

KeyTipController::KeyTipController(QWidget* host, QObject* parent)
    : QObject(parent), host_(host) {
    // 濾在 QApplication 上而不是 host 上：Alt 通常在焦點落在檢視元件或面板時按下，
    // 那些 widget 不會把按鍵往 Ribbon 送。belongsToHost() 負責把範圍收回同一個視窗。
    qApp->installEventFilter(this);
}

KeyTipController::~KeyTipController() {
    if (qApp != nullptr) qApp->removeEventFilter(this);
}

void KeyTipController::setTargets(QList<Target> targets) {
    targets_ = std::move(targets);
    if (showing_) {
        clearLabels();
        placeLabels();
    }
}

QStringList KeyTipController::visibleKeyTips() const {
    if (!showing_) return {};
    QStringList tips;
    tips.reserve(static_cast<int>(targets_.size()));
    for (const Target& target : targets_) tips << target.keyTip;
    return tips;
}

void KeyTipController::clearLabels() {
    qDeleteAll(labels_);
    labels_.clear();
}

void KeyTipController::placeLabels() {
    if (host_.isNull()) return;
    for (const Target& target : targets_) {
        if (target.keyTip.isEmpty() || target.anchorRect.isNull()) continue;
        auto* label = new QLabel(target.keyTip, host_);
        label->setFrameShape(QFrame::Box);
        label->setAlignment(Qt::AlignCenter);
        label->setAutoFillBackground(true);
        // 用 ToolTip 的配色：它在淺色與深色主題下都被系統定義為「浮在內容之上」的對比，
        // 自己挑顏色會在其中一種主題下讀不到。
        QPalette tipPalette = label->palette();
        tipPalette.setColor(QPalette::Window, host_->palette().color(QPalette::ToolTipBase));
        tipPalette.setColor(QPalette::WindowText, host_->palette().color(QPalette::ToolTipText));
        label->setPalette(tipPalette);
        label->adjustSize();

        const QPoint center = target.anchorRect.center();
        label->move(center.x() - label->width() / 2,
                    target.anchorRect.bottom() - label->height() / 2);
        label->show();
        label->raise();
        labels_.append(label);
    }
}

void KeyTipController::showTips() {
    if (host_.isNull()) return;
    showing_ = true;
    clearLabels();
    placeLabels();
}

void KeyTipController::hideTips() {
    if (!showing_) return;
    showing_ = false;
    clearLabels();
    emit dismissed();
}

bool KeyTipController::handleKey(QChar character) {
    const QChar upper = character.toUpper();
    for (const Target& target : targets_) {
        if (target.keyTip.isEmpty()) continue;
        if (target.keyTip.at(0).toUpper() != upper) continue;
        // 先複製再呼叫：activate 常常會重建 targets_（切分頁就是），
        // 在容器被改動之後才讀 target 會是懸空參考。
        const auto activate = target.activate;
        const QString keyTip = target.keyTip;
        if (activate) activate();
        emit keyTipActivated(keyTip);
        return true;
    }
    return false;
}

bool KeyTipController::belongsToHost(const QObject* watched) const {
    if (host_.isNull() || !host_->isVisible()) return false;
    const auto* widget = qobject_cast<const QWidget*>(watched);
    if (widget == nullptr) return false;
    const QWidget* window = host_->window();
    return widget == window || window->isAncestorOf(widget);
}

bool KeyTipController::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate && showing_) {
        hideTips();
        return false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return false;
    if (!belongsToHost(watched)) return false;

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->key() == Qt::Key_Alt) {
        // 只在放開時反應：按住 Alt 打 Alt+F 這種既有快捷鍵時不該閃出提示。
        if (event->type() == QEvent::KeyRelease && !keyEvent->isAutoRepeat() &&
            (keyEvent->modifiers() & ~Qt::AltModifier) == Qt::NoModifier) {
            if (showing_) {
                hideTips();
            } else {
                emit activationRequested();
            }
            return true;
        }
        return false;
    }

    if (!showing_ || event->type() != QEvent::KeyPress) return false;

    if (keyEvent->key() == Qt::Key_Escape) {
        emit backRequested();
        return true;
    }

    const QString text = keyEvent->text();
    if (text.isEmpty() || !text.at(0).isLetterOrNumber()) return false;
    if (handleKey(text.at(0))) return true;

    // 按到不存在的提示就整組關掉：留在半亮狀態會讓使用者以為鍵盤沒反應。
    hideTips();
    return true;
}

}  // namespace alioth::ui::ribbon
