#include "ui/ribbon/ribbon_bar.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
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

// 分頁內容的橫向捲動容器（PRD-UI-002）。
//
// 沒有它的時候，視窗窄於分頁的自然寬度，右側的群組就直接被裁掉——**沒有任何
// 提示，也沒有任何方式到得了那些按鈕**。鍵盤 Tab 仍然走得到（焦點會移到看不見
// 的地方），滑鼠則完全沒轍，而無障礙稽核只檢查「鍵盤可達」，所以掃不出來。
//
// 選擇捲動而不是 Office 那種「群組收成一顆彈出鈕」：後者要為每個群組設計
// 摺疊態的圖示與彈出版面，而且摺疊順序本身是個產品決策（哪個群組先讓位）。
// 捲動不需要任何額外設計，且保證每顆按鈕都到得了——那是這一條的驗收重點。
//
// 不繼承 Q_OBJECT：這個類別沒有自己的訊號或屬性，加了只是多一份 moc 產物，
// 而 .cpp 裡的 Q_OBJECT 還得自己 #include .moc。
class RibbonPageScroller : public QScrollArea {
public:
    explicit RibbonPageScroller(QWidget* parent = nullptr) : QScrollArea(parent) {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        // 縱向永遠不捲：Ribbon 的高度由內容決定，出現縱向捲軸代表版面算錯了，
        // 而一條擠在 Ribbon 裡的縱向捲軸比被裁掉更難看也更難用。
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        // 背景交給 Ribbon 自己畫，捲動區不要再疊一層底色。
        viewport()->setAutoFillBackground(false);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        // 捲動容器本身不收焦點：Ribbon 的鍵盤入口是 KeyTips（Alt），動作
        // 是否觸發與它看不看得見無關，所以這裡多一站只會讓 Tab 序多一個
        // 沒有用途的停留點。無障礙稽核也會把「會收焦點但角色是 Client」
        // 的容器視為缺失，而那個判斷是對的。
        setFocusPolicy(Qt::NoFocus);
    }

    // 高度跟著內容走，寬度只回一個下限——回內容的自然寬度會讓整個主視窗
    // 被 Ribbon 撐開，那等於用另一種方式讓使用者無法把視窗縮小。
    [[nodiscard]] QSize sizeHint() const override {
        QWidget* content = widget();
        if (content == nullptr) return QScrollArea::sizeHint();
        int height = content->sizeHint().height();
        // 依「內容放不放得下」預留捲軸高度，而不是依「捲軸現在可不可見」：
        // 後者是雞生蛋——版面要先問高度才會決定寬度，而捲軸要等寬度定了
        // 才會出現，於是第一輪永遠算成不需要。症狀是捲軸壓在群組標題上，
        // 看起來像最後一排文字被切掉一半。
        // 捲軸的高度**一律預留**，不論此刻需不需要。
        //
        // 依實際需要動態加減試過，結果是 Ribbon 的高度會跟著視窗寬度跳動：
        // 拖動視窗邊緣時，整個文件檢視區在某個寬度上突然往下移十幾個像素，
        // 而使用者拖的是「寬度」。固定高度換來的是可預期的版面，代價只是
        // 放得下的時候底部多一條十幾像素的空白——那條空白看起來就是邊距。
        if (horizontalScrollBar() != nullptr) {
            height += horizontalScrollBar()->sizeHint().height();
        }
        return QSize(0, height);
    }

    [[nodiscard]] QSize minimumSizeHint() const override {
        return QSize(0, sizeHint().height());
    }

protected:
    // 捲軸出現或消失會改變需要的高度。不重新通知版面的話，捲軸會蓋住
    // 最後一排按鈕的下緣——看起來像按鈕被切掉一半。
    void resizeEvent(QResizeEvent* event) override {
        QScrollArea::resizeEvent(event);
        updateGeometry();
    }
};

// 快速存取列。存在的唯一理由是打破一個循環：
//
// QWidget::minimumSizeHint() 取自版面的 totalMinimumSize，也就是所有可見按鈕
// 的寬度總和；而 QAT 在 Ribbon 的外層版面裡，那個總和於是變成整個主視窗的
// 最小寬度。溢位邏輯只在 resize 時才把放不下的按鈕藏起來——但 resize 本身
// 被那個最小寬度擋住了，所以按鈕永遠不會藏，視窗也永遠縮不小。
//
// 回 0 是安全的：真正需要的高度仍然照實回報，而寬度不足時的處置
// （收進「»」選單）已經有了。
class QuickAccessBar : public QWidget {
public:
    explicit QuickAccessBar(QWidget* parent = nullptr) : QWidget(parent) {}

    [[nodiscard]] QSize minimumSizeHint() const override {
        return QSize(0, QWidget::minimumSizeHint().height());
    }
};

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

    quickAccessBar_ = new QuickAccessBar(this);
    quickAccessLayout_ = new QHBoxLayout(quickAccessBar_);
    quickAccessLayout_->setContentsMargins(unit, unit / 2, unit, 0);
    quickAccessLayout_->setSpacing(unit / 2);
    // 不讓版面把「所有按鈕的總寬」當成硬性下限：那會直接變成整個主視窗的
    // 最小寬度，使用者再也縮不小，而按鈕多寡本來是使用者自己決定的。
    // 放不下的部分改走「»」溢位選單，見 updateQuickAccessOverflow()。
    quickAccessLayout_->setSizeConstraint(QLayout::SetNoConstraint);

    quickAccessOverflow_ = new QToolButton(quickAccessBar_);
    quickAccessOverflow_->setObjectName(QStringLiteral("ribbonQuickAccessOverflow"));
    quickAccessOverflow_->setText(QStringLiteral("»"));
    quickAccessOverflow_->setToolTip(QObject::tr("其餘快速存取項目"));
    quickAccessOverflow_->setAccessibleName(QObject::tr("其餘快速存取項目"));
    quickAccessOverflow_->setPopupMode(QToolButton::InstantPopup);
    quickAccessOverflow_->setMenu(new QMenu(quickAccessOverflow_));
    quickAccessOverflow_->hide();

    quickAccessLayout_->addStretch(1);
    quickAccessLayout_->addWidget(quickAccessOverflow_);
    quickAccessBar_->setMinimumWidth(0);
    quickAccessBar_->installEventFilter(this);
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
        // 分頁本身放進捲動容器再入 stack。pages_ 仍然存 RibbonPageWidget，
        // 所有既有的查詢（群組、按鈕、KeyTip）因此完全不受影響。
        auto* scroller = new RibbonPageScroller(stack_);
        // objectName 是 UIA 的 AutomationId 來源，同一條規則適用於容器。
        scroller->setObjectName(QStringLiteral("ribbonPageScroll_") + page.id);
        auto* widget = new RibbonPageWidget(page, registry_, iconResolver_, scroller);
        scroller->setWidget(widget);
        pages_.append(widget);
        stack_->addWidget(scroller);
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
        // 插在 stretch 與溢位鈕之前，讓按鈕靠左。
        quickAccessLayout_->insertWidget(quickAccessLayout_->count() - 2, button);
        quickAccessButtons_.append(button);
        ++index;
    }
    quickAccessBar_->setVisible(!quickAccessButtons_.isEmpty());
    updateQuickAccessOverflow();
}

void RibbonBar::updateQuickAccessOverflow() {
    if (quickAccessBar_ == nullptr || quickAccessOverflow_ == nullptr) return;

    QMenu* menu = quickAccessOverflow_->menu();
    if (menu != nullptr) menu->clear();

    const QMargins margins = quickAccessLayout_->contentsMargins();
    const int spacing = std::max(0, quickAccessLayout_->spacing());
    const int overflowWidth = quickAccessOverflow_->sizeHint().width();
    int available = quickAccessBar_->width() - margins.left() - margins.right();

    // 先算「全部都放得下嗎」。放得下就不留溢位鈕——一顆永遠在那裡但按下去
    // 是空選單的按鈕，比沒有更糟。
    int needed = 0;
    for (const RibbonButton* button : quickAccessButtons_) {
        if (needed > 0) needed += spacing;
        needed += button->sizeHint().width();
    }
    const bool overflows = needed > available;
    if (overflows) available -= overflowWidth + spacing;

    int used = 0;
    QList<RibbonButton*> hidden;
    for (RibbonButton* button : quickAccessButtons_) {
        const int width = button->sizeHint().width() + (used > 0 ? spacing : 0);
        // 至少留一顆看得見：可用寬度小到連第一顆都放不下時，全部收進選單會
        // 讓那一列只剩一個「»」，使用者看不出那裡本來有東西。
        const bool fits = !overflows || used + width <= available || used == 0;
        button->setVisible(fits);
        if (fits) {
            used += width;
        } else {
            hidden.append(button);
        }
    }

    for (RibbonButton* button : hidden) {
        QAction* action = button->defaultAction();
        if (action != nullptr) {
            if (menu != nullptr) menu->addAction(action);
            continue;
        }
        // 沒有註冊動作的佔位鈕也要列出來，否則它會在窄視窗下整個消失，
        // 而「按鈕不見了」與「按鈕停用」是完全不同的訊息。
        if (menu != nullptr) {
            QAction* placeholder = menu->addAction(button->text());
            placeholder->setEnabled(false);
        }
    }
    quickAccessOverflow_->setVisible(!hidden.isEmpty());
}

bool RibbonBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == quickAccessBar_ && event->type() == QEvent::Resize) {
        updateQuickAccessOverflow();
    }
    return QWidget::eventFilter(watched, event);
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
    // 觸控模式把每顆按鈕撐大，同一條 QAT 能放下的數量因此改變。
    // 少了這一行，切到觸控模式之後多出來的按鈕會直接被裁掉而不是進溢位選單。
    updateQuickAccessOverflow();
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
