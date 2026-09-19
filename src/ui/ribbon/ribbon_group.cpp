#include "ui/ribbon/ribbon_group.h"

#include <QMenu>

#include <QAction>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_button.h"

namespace alioth::ui::ribbon {
namespace {

// 小按鈕一欄最多三顆——這是 Office 系 Ribbon 的既定節奏，也剛好等於一顆大按鈕的高度。
constexpr int kSmallItemsPerColumn = 3;

// 間距同樣以字高為單位，理由與 RibbonButton 相同：跟著字型走才不會在高 DPI 下錯位。
[[nodiscard]] int spacingUnit(const QWidget* widget) {
    return std::max(2, QFontMetrics(widget->font()).height() / 4);
}

}  // namespace

RibbonGroupWidget::RibbonGroupWidget(const Group& group, ActionRegistry* registry,
                                     IconResolver iconResolver, QWidget* parent)
    : QWidget(parent),
      registry_(registry),
      iconResolver_(std::move(iconResolver)),
      id_(group.id) {
    setObjectName(group.id);
    build(group);

    if (registry_ != nullptr) {
        // 整合端可能在 Ribbon 建好之後才注入動作（例如開檔後才存在的功能）。
        // 沒有這兩條，晚到的動作會永遠停在停用的佔位狀態。
        connect(registry_, &ActionRegistry::actionRegistered, this,
                [this](const QString& id) {
                    if (RibbonButton* button = buttonForActionId(id); button != nullptr) {
                        rebind(button);
                    }
                });
        connect(registry_, &ActionRegistry::actionUnregistered, this,
                [this](const QString& id) {
                    if (RibbonButton* button = buttonForActionId(id); button != nullptr) {
                        rebind(button);
                    }
                });
    }
}

RibbonButton* RibbonGroupWidget::makeButton(const Item& item) {
    auto* button = new RibbonButton(item, this);
    items_.insert(item.actionId, item);
    rebind(button);
    return button;
}

void RibbonGroupWidget::rebind(RibbonButton* button) {
    const QString id = button->actionId();
    const Item item = items_.value(id);
    QAction* action = registry_ != nullptr ? registry_->action(id) : nullptr;

    // 每次重綁都先切斷舊連線：動作被替換時若留著舊的，一次點擊會觸發兩個動作。
    disconnect(button, nullptr, this, nullptr);
    if (boundActions_.contains(id) && !boundActions_.value(id).isNull()) {
        disconnect(boundActions_.value(id), nullptr, this, nullptr);
    }
    boundActions_.insert(id, action);

    const QString label = !item.label.isEmpty() ? item.label
                          : action != nullptr   ? action->text()
                                                : id;
    button->setText(label);

    QIcon icon;
    if (action != nullptr && !action->icon().isNull()) {
        icon = action->icon();
    } else if (iconResolver_) {
        icon = iconResolver_(item.iconName);
    }
    button->setIcon(icon);

    if (action == nullptr) {
        button->setPlaceholder(true);
        return;
    }

    button->setPlaceholder(false);

    // 指向子選單的動作要真的彈出選單。
    //
    // QMenu::menuAction() 是一個合法的 QAction，註冊起來一切正常，但
    // trigger() 對它只會發出 triggered——彈出選單是 QMenuBar 或
    // QToolButton::setMenu 的行為，不是動作本身的。少了這一段，「最近使用」
    // 「管理設定」「設定狀態」在 Ribbon 上按下去完全沒有反應，而在傳統選單
    // 裡一切正常，於是這個缺陷只在預設介面上出現。
    if (QMenu* menu = action->menu(); menu != nullptr) {
        button->setMenu(menu);
        button->setPopupMode(QToolButton::InstantPopup);
    } else {
        button->setMenu(nullptr);
        button->setPopupMode(QToolButton::DelayedPopup);
    }
    button->setEnabled(action->isEnabled());
    button->setCheckable(action->isCheckable());
    button->setChecked(action->isChecked());
    button->setToolTip(action->toolTip());
    button->setShortcut(action->shortcut());

    // 刻意不用 setDefaultAction：那會讓 QAction 的文字反過來覆蓋設定檔裡的自訂標籤，
    // 而自訂標籤正是 PRD-UI-011 的重點。改成手動同步狀態，代價是要自己接這幾條線。
    // 一律走 trigger()：可勾選的動作靠下面那條 changed 把狀態同步回按鈕，
    // 若在這裡直接 setChecked()，QAction::triggered 不會發出，
    // 整合端就收不到「使用者按了這個 id」——那是 Ribbon 對外唯一的事件來源。
    connect(button, &QToolButton::clicked, this, [action] { action->trigger(); });
    connect(action, &QAction::changed, this, [this, button, id] {
        QAction* current = registry_ != nullptr ? registry_->action(id) : nullptr;
        if (current == nullptr) return;
        button->setEnabled(current->isEnabled());
        button->setCheckable(current->isCheckable());
        button->setChecked(current->isChecked());
    });
}

void RibbonGroupWidget::build(const Group& group) {
    const int unit = spacingUnit(this);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(unit, unit, unit, unit / 2);
    outer->setSpacing(unit / 2);

    contentLayout_ = new QHBoxLayout;
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(unit);
    outer->addLayout(contentLayout_, 1);

    QVBoxLayout* column = nullptr;
    int inColumn = 0;
    const auto closeColumn = [&] {
        if (column != nullptr) column->addStretch(1);
        column = nullptr;
        inColumn = 0;
    };

    for (const Item& item : group.items) {
        if (item.type == ItemType::Separator) {
            closeColumn();
            auto* line = new QFrame(this);
            line->setFrameShape(QFrame::VLine);
            line->setFrameShadow(QFrame::Plain);
            contentLayout_->addWidget(line);
            continue;
        }

        RibbonButton* button = makeButton(item);
        buttons_.append(button);

        if (item.size == ItemSize::Large) {
            closeColumn();
            contentLayout_->addWidget(button, 0, Qt::AlignTop);
            continue;
        }

        if (column == nullptr || inColumn >= kSmallItemsPerColumn) {
            closeColumn();
            column = new QVBoxLayout;
            column->setContentsMargins(0, 0, 0, 0);
            column->setSpacing(0);
            contentLayout_->addLayout(column);
        }
        column->addWidget(button);
        ++inColumn;
    }
    closeColumn();
    contentLayout_->addStretch(1);

    titleLabel_ = new QLabel(group.title, this);
    titleLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    applyTitlePalette();
    outer->addWidget(titleLabel_);
}

void RibbonGroupWidget::applyTitlePalette() {
    if (titleLabel_ == nullptr) return;
    // 標題用 Disabled 前景色而非寫死的灰：淺色與深色主題下都會自動落在正確的對比。
    QPalette titlePalette = titleLabel_->palette();
    titlePalette.setColor(QPalette::WindowText,
                          palette().color(QPalette::Disabled, QPalette::WindowText));
    titleLabel_->setPalette(titlePalette);
}

void RibbonGroupWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // 主題切換時 palette 會換一整組，標題色若不重算就會留在舊主題的對比上。
    if (event->type() == QEvent::PaletteChange) applyTitlePalette();
}

QString RibbonGroupWidget::title() const {
    return titleLabel_ != nullptr ? titleLabel_->text() : QString{};
}

RibbonButton* RibbonGroupWidget::buttonForActionId(const QString& actionId) const {
    for (RibbonButton* button : buttons_) {
        if (button->actionId() == actionId) return button;
    }
    return nullptr;
}

void RibbonGroupWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    // 分隔線取 palette 的 Mid，深色主題下會自動變成比背景亮的線，不必判斷主題。
    painter.setPen(palette().color(QPalette::Mid));
    const int unit = spacingUnit(this);
    painter.drawLine(width() - 1, unit, width() - 1, height() - unit);
}

}  // namespace alioth::ui::ribbon
