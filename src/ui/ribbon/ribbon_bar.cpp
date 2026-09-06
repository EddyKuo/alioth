#include "ui/ribbon/ribbon_bar.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/key_tip_controller.h"
#include "ui/ribbon/ribbon_button.h"
#include "ui/ribbon/ribbon_group.h"
#include "ui/ribbon/ribbon_page.h"

namespace alioth::ui::ribbon {
namespace {

[[nodiscard]] bool withinDoubleClickWindow(const QElapsedTimer& timer) {
    return timer.isValid() && timer.elapsed() < QApplication::doubleClickInterval();
}

// 快速存取列的 KeyTip 用數字，與分頁的字母錯開，避免第 0 層出現同鍵衝突。
[[nodiscard]] QString quickAccessKeyTip(int index) {
    return index < 9 ? QString::number(index + 1) : QString{};
}

}  // namespace

RibbonBar::RibbonBar(ActionRegistry* registry, QWidget* parent)
    : QWidget(parent), registry_(registry), iconResolver_(&resolveIconByName) {
    buildChrome();

    if (registry_ != nullptr) {
        connect(registry_, &ActionRegistry::actionTriggered, this, &RibbonBar::actionTriggered);
    }
}

void RibbonBar::buildChrome() {
    const int unit = std::max(2, QFontMetrics(font()).height() / 4);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    quickAccessBar_ = new QWidget(this);
    quickAccessLayout_ = new QHBoxLayout(quickAccessBar_);
    quickAccessLayout_->setContentsMargins(unit, unit / 2, unit, 0);
    quickAccessLayout_->setSpacing(unit / 2);
    quickAccessLayout_->addStretch(1);
    outer->addWidget(quickAccessBar_);

    tabBar_ = new QTabBar(this);
    // objectName 是 UIA 的 AutomationId 來源。沒有它，自動化測試與螢幕閱讀器
    // 都只看得到一個匿名節點。
    tabBar_->setObjectName(QStringLiteral("ribbonTabBar"));
    tabBar_->setAccessibleName(QObject::tr("Ribbon 分頁"));
    tabBar_->setDrawBase(false);
    tabBar_->setExpanding(false);
    tabBar_->setFocusPolicy(Qt::TabFocus);
    auto* tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(unit, 0, unit, 0);
    tabRow->addWidget(tabBar_);
    tabRow->addStretch(1);
    outer->addLayout(tabRow);

    stack_ = new QStackedWidget(this);
    // Ribbon 內容高度由內容決定，橫向填滿；少了這行，收合時 Ribbon 不會真的變矮。
    stack_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    outer->addWidget(stack_);

    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // 背景取自 palette 而非固定色，深色主題會跟著換。
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);

    keyTips_ = new KeyTipController(this, this);
    connect(keyTips_, &KeyTipController::activationRequested, this, [this] {
        setKeyTipLevel(0);
        keyTips_->showTips();
    });
    connect(keyTips_, &KeyTipController::backRequested, this, [this] {
        if (keyTipLevel_ > 0) {
            setKeyTipLevel(0);
            keyTips_->showTips();
        } else {
            keyTips_->hideTips();
        }
    });

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        if (stack_ != nullptr) stack_->setCurrentIndex(index);
        emit currentPageChanged(index);
    });
    // 單擊與雙擊是同一個手勢的兩個訊號，而它們的先後順序在不同平台（甚至不同的
    // Qt 平台外掛）並不一致——offscreen 上雙擊先到，Windows 原生上單擊先到。
    // 因此只能以「兩者相距不到一個雙擊間隔就算同一個手勢」來去重，
    // 靠訊號順序寫出來的版本會在其中一個平台上變成收合後立刻自己展開。
    connect(tabBar_, &QTabBar::tabBarClicked, this, [this](int) {
        if (withinDoubleClickWindow(sinceDoubleClick_)) return;
        // 收合狀態下單擊分頁即展開（PRD-UI-002 的「再點展開」）。
        if (!collapsed_) return;
        setCollapsed(false);
        sinceClickExpand_.start();
    });
    connect(tabBar_, &QTabBar::tabBarDoubleClicked, this, [this](int) {
        const bool justExpanded = withinDoubleClickWindow(sinceClickExpand_);
        sinceDoubleClick_.start();
        // 收合狀態下雙擊：前半的單擊已經展開了，這裡再收回去等於什麼都沒發生。
        if (justExpanded) return;
        setCollapsed(!collapsed_);
    });
}

void RibbonBar::setIconResolver(IconResolver resolver) {
    iconResolver_ = resolver ? std::move(resolver) : IconResolver(&resolveIconByName);
}

void RibbonBar::setLayoutModel(const Layout& layout) {
    layout_ = layout;
    effective_ = layout;
    assignAutomaticKeyTips(effective_);
    rebuild();
    // rebuild() 造出來的是全新的按鈕，觸控模式要重新套上去；
    // 少了這一行，換配置之後整條 Ribbon 會靜靜地退回滑鼠尺寸。
    if (touchMode_) {
        for (RibbonButton* button : findChildren<RibbonButton*>()) button->setTouchMode(true);
    }
    setCollapsed(layout_.collapsed);
    emit layoutModelChanged();
}

void RibbonBar::rebuild() {
    keyTips_->hideTips();
    keyTipLevel_ = 0;

    // 先清空分頁列再清 stack：反過來的話 currentChanged 會在 stack 已空時
    // 被 QTabBar 的 removeTab 觸發，落到一個索引已經失效的分頁上。
    {
        const QSignalBlocker blocker(tabBar_);
        while (tabBar_->count() > 0) tabBar_->removeTab(0);
    }
    while (stack_->count() > 0) {
        QWidget* widget = stack_->widget(0);
        stack_->removeWidget(widget);
        widget->deleteLater();
    }
    pages_.clear();

    for (const Page& page : effective_.pages) {
        auto* widget = new RibbonPageWidget(page, registry_, iconResolver_, stack_);
        pages_.append(widget);
        stack_->addWidget(widget);
        const QString title = page.title.isEmpty() ? page.id : page.title;
        const int index = tabBar_->addTab(title);
        tabBar_->setTabData(index, page.id);
    }
    if (!pages_.isEmpty()) {
        tabBar_->setCurrentIndex(0);
        stack_->setCurrentIndex(0);
    }

    rebuildQuickAccess();
    // 空配置是合法狀態（使用者可能把所有分頁都刪了），此時只留下分頁列與 QAT 的骨架。
    stack_->setVisible(!collapsed_ && !pages_.isEmpty());
}

void RibbonBar::rebuildQuickAccess() {
    qDeleteAll(quickAccessButtons_);
    quickAccessButtons_.clear();

    int index = 0;
    for (const QString& id : effective_.quickAccessActionIds) {
        if (id.isEmpty()) continue;
        Item item;
        item.actionId = id;
        item.size = ItemSize::Small;
        item.keyTip = quickAccessKeyTip(index);

        auto* button = new RibbonButton(item, quickAccessBar_);
        QAction* action = registry_ != nullptr ? registry_->action(id) : nullptr;
        if (action != nullptr) {
            button->setDefaultAction(action);
            const QIcon icon = action->icon().isNull() && iconResolver_ ? iconResolver_(id)
                                                                       : action->icon();
            button->setIcon(icon);
            // 快速存取列空間很窄，有圖示就只留圖示，文字退到 tooltip。
            button->setToolButtonStyle(icon.isNull() ? Qt::ToolButtonTextOnly
                                                     : Qt::ToolButtonIconOnly);
            button->setToolTip(action->toolTip().isEmpty() ? action->text() : action->toolTip());
        } else {
            button->setText(id);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setPlaceholder(true);
        }
        // 插在 stretch 之前，讓按鈕靠左。
        quickAccessLayout_->insertWidget(quickAccessLayout_->count() - 1, button);
        quickAccessButtons_.append(button);
        ++index;
    }
    quickAccessBar_->setVisible(!quickAccessButtons_.isEmpty());
}

int RibbonBar::pageCount() const { return static_cast<int>(pages_.size()); }

RibbonPageWidget* RibbonBar::page(int index) const {
    if (index < 0 || index >= pages_.size()) return nullptr;
    return pages_.at(index);
}

RibbonPageWidget* RibbonBar::pageById(const QString& pageId) const {
    for (RibbonPageWidget* page : pages_) {
        if (page->pageId() == pageId) return page;
    }
    return nullptr;
}

int RibbonBar::currentPageIndex() const { return tabBar_->currentIndex(); }

QString RibbonBar::currentPageId() const {
    RibbonPageWidget* current = page(currentPageIndex());
    return current != nullptr ? current->pageId() : QString{};
}

void RibbonBar::setCurrentPageIndex(int index) {
    if (index < 0 || index >= pages_.size()) return;
    tabBar_->setCurrentIndex(index);
}

bool RibbonBar::setCurrentPageId(const QString& pageId) {
    for (int i = 0; i < pages_.size(); ++i) {
        if (pages_.at(i)->pageId() == pageId) {
            setCurrentPageIndex(i);
            return true;
        }
    }
    return false;
}

void RibbonBar::setCollapsed(bool collapsed) {
    if (collapsed_ == collapsed) return;
    collapsed_ = collapsed;
    layout_.collapsed = collapsed;
    effective_.collapsed = collapsed;
    stack_->setVisible(!collapsed_ && !pages_.isEmpty());
    updateGeometry();
    emit collapsedChanged(collapsed_);
}

void RibbonBar::setTouchMode(bool enabled) {
    if (touchMode_ == enabled) return;
    touchMode_ = enabled;
    for (RibbonButton* button : findChildren<RibbonButton*>()) button->setTouchMode(enabled);
    updateGeometry();
}

RibbonButton* RibbonBar::buttonForActionId(const QString& actionId) const {
    for (RibbonPageWidget* page : pages_) {
        if (RibbonButton* button = page->buttonForActionId(actionId); button != nullptr) {
            return button;
        }
    }
    for (RibbonButton* button : quickAccessButtons_) {
        if (button->actionId() == actionId) return button;
    }
    return nullptr;
}

QStringList RibbonBar::missingActionIds() const {
    if (registry_ == nullptr) return collectActionIds(layout_);
    return registry_->missingIds(collectActionIds(layout_));
}

void RibbonBar::setKeyTipLevel(int level) {
    keyTipLevel_ = level;
    updateKeyTipTargets();
}

void RibbonBar::updateKeyTipTargets() {
    QList<KeyTipController::Target> targets;

    if (keyTipLevel_ == 0) {
        for (int i = 0; i < effective_.pages.size() && i < tabBar_->count(); ++i) {
            const QString keyTip = effective_.pages.at(i).keyTip;
            if (keyTip.isEmpty()) continue;
            const QRect rect = tabBar_->tabRect(i).translated(tabBar_->mapTo(this, QPoint(0, 0)));
            targets.append({keyTip, rect, [this, i] {
                                setCurrentPageIndex(i);
                                // 進第一層之後要展開，否則提示指向的是看不見的按鈕。
                                if (collapsed_) setCollapsed(false);
                                setKeyTipLevel(1);
                                keyTips_->showTips();
                            }});
        }
        for (int i = 0; i < quickAccessButtons_.size(); ++i) {
            RibbonButton* button = quickAccessButtons_.at(i);
            const QString keyTip = quickAccessKeyTip(i);
            if (keyTip.isEmpty()) continue;
            const QRect rect = QRect(button->mapTo(this, QPoint(0, 0)), button->size());
            targets.append({keyTip, rect, [this, button] {
                                button->click();
                                keyTips_->hideTips();
                            }});
        }
    } else {
        RibbonPageWidget* current = page(currentPageIndex());
        const int pageIndex = currentPageIndex();
        if (current != nullptr && pageIndex >= 0 && pageIndex < effective_.pages.size()) {
            const Page& model = effective_.pages.at(pageIndex);
            for (const Group& group : model.groups) {
                for (const Item& item : group.items) {
                    if (item.type != ItemType::Action || item.keyTip.isEmpty()) continue;
                    RibbonButton* button = current->buttonForActionId(item.actionId);
                    if (button == nullptr || !button->isEnabled()) continue;
                    const QRect rect = QRect(button->mapTo(this, QPoint(0, 0)), button->size());
                    targets.append({item.keyTip, rect, [this, button] {
                                        button->click();
                                        keyTips_->hideTips();
                                    }});
                }
            }
        }
    }
    keyTips_->setTargets(std::move(targets));
}

void RibbonBar::showKeyTips() {
    setKeyTipLevel(0);
    keyTips_->showTips();
}

void RibbonBar::hideKeyTips() { keyTips_->hideTips(); }

bool RibbonBar::keyTipsVisible() const { return keyTips_->isShowing(); }

QStringList RibbonBar::visibleKeyTips() const { return keyTips_->visibleKeyTips(); }

bool RibbonBar::handleKeyTip(QChar character) {
    if (!keyTips_->isShowing()) return false;
    return keyTips_->handleKey(character);
}

}  // namespace alioth::ui::ribbon
