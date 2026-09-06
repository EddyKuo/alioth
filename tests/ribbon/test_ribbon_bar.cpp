// Ribbon 元件的測試（WBS 3.2 / PRD-UI-002）。
//
// 全部用假的 QAction 注入：Ribbon 對外只認 actionId，所以驗證它不需要文件、
// 引擎或主視窗。這正是把動作做成注入點的目的，也讓這個測試可以在 offscreen 上跑。

#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTabBar>

#include "app/touch/touch_gestures.h"
#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_bar.h"
#include "ui/ribbon/ribbon_button.h"
#include "ui/ribbon/ribbon_default_layout.h"
#include "ui/ribbon/ribbon_group.h"
#include "ui/ribbon/ribbon_model.h"
#include "ui/ribbon/ribbon_page.h"

using namespace alioth::ui::ribbon;

namespace {

[[nodiscard]] Item action(const QString& id, ItemSize size = ItemSize::Large,
                          const QString& keyTip = QString{}) {
    Item item;
    item.actionId = id;
    item.label = id;
    item.size = size;
    item.keyTip = keyTip;
    return item;
}

// 兩個分頁：home 有兩個群組共三個項目，view 有一個群組兩個項目。
[[nodiscard]] Layout twoPageLayout() {
    Group clipboard;
    clipboard.id = QStringLiteral("home.clipboard");
    clipboard.title = QStringLiteral("剪貼簿");
    clipboard.items = {action(QStringLiteral("edit.copy"), ItemSize::Large, QStringLiteral("C")),
                       Item{ItemType::Separator, {}, {}, {}, ItemSize::Large, {}},
                       action(QStringLiteral("edit.paste"), ItemSize::Small,
                              QStringLiteral("P"))};

    Group tools;
    tools.id = QStringLiteral("home.tools");
    tools.title = QStringLiteral("工具");
    tools.items = {action(QStringLiteral("tool.select"), ItemSize::Small, QStringLiteral("S"))};

    Page home;
    home.id = QStringLiteral("home");
    home.title = QStringLiteral("常用");
    home.keyTip = QStringLiteral("H");
    home.groups = {clipboard, tools};

    Group zoom;
    zoom.id = QStringLiteral("view.zoom");
    zoom.title = QStringLiteral("縮放");
    zoom.items = {action(QStringLiteral("view.zoomIn"), ItemSize::Large, QStringLiteral("I")),
                  action(QStringLiteral("view.zoomOut"), ItemSize::Large, QStringLiteral("O"))};

    Page view;
    view.id = QStringLiteral("view");
    view.title = QStringLiteral("檢視");
    view.keyTip = QStringLiteral("V");
    view.groups = {zoom};

    Layout layout;
    layout.pages = {home, view};
    layout.quickAccessActionIds = {QStringLiteral("file.save")};
    return layout;
}

}  // namespace

class TestRibbonBar : public QObject {
    Q_OBJECT

private:
    // 依配置注入假的 QAction，讓按鈕都是實際可按的。
    void injectActions(ActionRegistry& registry, const Layout& layout, QObject* owner) {
        for (const QString& id : collectActionIds(layout)) {
            auto* fake = new QAction(id, owner);
            registry.registerAction(id, fake);
        }
    }

private slots:
    void buildsPagesGroupsAndItemsFromModel() {
        ActionRegistry registry;
        QObject owner;
        const Layout layout = twoPageLayout();
        injectActions(registry, layout, &owner);

        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);

        QCOMPARE(bar.pageCount(), 2);
        QCOMPARE(bar.tabBar()->count(), 2);
        QCOMPARE(bar.page(0)->pageId(), QStringLiteral("home"));
        QCOMPARE(bar.page(0)->groupCount(), 2);
        // 分隔線不是項目，不該被算進去。
        QCOMPARE(bar.page(0)->itemCount(), 3);
        QCOMPARE(bar.page(0)->group(0)->title(), QStringLiteral("剪貼簿"));
        QCOMPARE(bar.page(1)->groupCount(), 1);
        QCOMPARE(bar.page(1)->itemCount(), 2);
        QCOMPARE(bar.quickAccessButtons().size(), 1);
        QVERIFY(bar.missingActionIds().isEmpty());
    }

    void defaultLayoutBuildsAllEightPages() {
        ActionRegistry registry;
        RibbonBar bar(&registry);
        bar.setLayoutModel(defaultLayout());

        QCOMPARE(bar.pageCount(), 8);
        QCOMPARE(bar.currentPageId(), QStringLiteral("file"));
        for (int i = 0; i < bar.pageCount(); ++i) {
            QVERIFY2(bar.page(i)->groupCount() > 0, qPrintable(bar.page(i)->pageId()));
            QVERIFY2(bar.page(i)->itemCount() > 0, qPrintable(bar.page(i)->pageId()));
        }
        // 一個動作都沒注入時，按鈕以停用的佔位呈現而不是消失。
        QVERIFY(!bar.missingActionIds().isEmpty());
        RibbonButton* button = bar.buttonForActionId(QStringLiteral("view.zoomIn"));
        QVERIFY(button != nullptr);
        QVERIFY(button->isPlaceholder());
        QVERIFY(!button->isEnabled());
    }

    void switchingPageShowsOnlyThatPagesItems() {
        ActionRegistry registry;
        QObject owner;
        const Layout layout = twoPageLayout();
        injectActions(registry, layout, &owner);

        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);
        bar.show();
        QTest::qWait(50);

        RibbonButton* copy = bar.page(0)->buttonForActionId(QStringLiteral("edit.copy"));
        RibbonButton* zoomIn = bar.page(1)->buttonForActionId(QStringLiteral("view.zoomIn"));
        QVERIFY(copy != nullptr);
        QVERIFY(zoomIn != nullptr);

        QVERIFY(copy->isVisible());
        QVERIFY(!zoomIn->isVisible());

        QSignalSpy pageChanged(&bar, &RibbonBar::currentPageChanged);
        bar.setCurrentPageIndex(1);
        QCOMPARE(pageChanged.count(), 1);
        QCOMPARE(bar.currentPageId(), QStringLiteral("view"));
        QVERIFY(!copy->isVisible());
        QVERIFY(zoomIn->isVisible());

        QVERIFY(bar.setCurrentPageId(QStringLiteral("home")));
        QVERIFY(copy->isVisible());
        QVERIFY(!zoomIn->isVisible());
        // 不存在的分頁 id 不改變狀態，也不崩潰。
        QVERIFY(!bar.setCurrentPageId(QStringLiteral("nope")));
        QCOMPARE(bar.currentPageId(), QStringLiteral("home"));
    }

    void collapseAndExpandTrackState() {
        ActionRegistry registry;
        QObject owner;
        const Layout layout = twoPageLayout();
        injectActions(registry, layout, &owner);

        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);
        bar.show();
        QTest::qWait(50);

        QSignalSpy collapsed(&bar, &RibbonBar::collapsedChanged);
        QVERIFY(!bar.isCollapsed());
        RibbonButton* copy = bar.page(0)->buttonForActionId(QStringLiteral("edit.copy"));
        QVERIFY(copy->isVisible());

        // 雙擊分頁標題收起。
        const QPoint tabCenter = bar.tabBar()->tabRect(0).center();
        QTest::mouseDClick(bar.tabBar(), Qt::LeftButton, Qt::NoModifier, tabCenter);
        QCOMPARE(collapsed.count(), 1);
        QVERIFY(bar.isCollapsed());
        QVERIFY(!copy->isVisible());

        // 收起後單擊分頁再展開。等過一個雙擊間隔，否則這一下會被視為前一個雙擊手勢的
        // 一部分而被忽略——那正是收合邏輯要防的情況。
        QTest::qWait(QApplication::doubleClickInterval() + 20);
        QTest::mouseClick(bar.tabBar(), Qt::LeftButton, Qt::NoModifier, tabCenter);
        QCOMPARE(collapsed.count(), 2);
        QVERIFY(!bar.isCollapsed());
        QVERIFY(copy->isVisible());

        // 收合狀態要能存回設定檔。
        bar.setCollapsed(true);
        QVERIFY(bar.layoutModel().collapsed);
        bar.setCollapsed(true);
        QCOMPARE(collapsed.count(), 3);
    }

    void clickingItemTriggersInjectedAction() {
        ActionRegistry registry;
        QObject owner;
        const Layout layout = twoPageLayout();
        injectActions(registry, layout, &owner);

        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);

        QSignalSpy relayed(&bar, &RibbonBar::actionTriggered);
        QSignalSpy fired(registry.action(QStringLiteral("edit.copy")), &QAction::triggered);

        bar.buttonForActionId(QStringLiteral("edit.copy"))->click();

        QCOMPARE(fired.count(), 1);
        QCOMPARE(relayed.count(), 1);
        QCOMPARE(relayed.takeFirst().at(0).toString(), QStringLiteral("edit.copy"));

        // 快速存取列走同一條轉發線。
        bar.quickAccessButtons().at(0)->click();
        QCOMPARE(relayed.count(), 1);
        QCOMPARE(relayed.takeFirst().at(0).toString(), QStringLiteral("file.save"));
    }

    void lateRegisteredActionRebindsPlaceholder() {
        ActionRegistry registry;
        RibbonBar bar(&registry);
        bar.setLayoutModel(twoPageLayout());

        RibbonButton* copy = bar.buttonForActionId(QStringLiteral("edit.copy"));
        QVERIFY(copy->isPlaceholder());

        QAction late(QStringLiteral("複製"));
        QVERIFY(!registry.registerAction(QStringLiteral("edit.copy"), &late));
        QVERIFY(!copy->isPlaceholder());
        QVERIFY(copy->isEnabled());
        // 設定檔裡的自訂標籤優先於 QAction 的文字，否則自訂 Ribbon 的改名會失效。
        QCOMPARE(copy->text(), QStringLiteral("edit.copy"));

        QSignalSpy fired(&late, &QAction::triggered);
        copy->click();
        QCOMPARE(fired.count(), 1);
    }

    void keyTipsWalkTwoLevels() {
        ActionRegistry registry;
        QObject owner;
        const Layout layout = twoPageLayout();
        injectActions(registry, layout, &owner);

        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);
        bar.show();
        QTest::qWait(50);

        QVERIFY(!bar.keyTipsVisible());
        bar.showKeyTips();
        QVERIFY(bar.keyTipsVisible());
        QCOMPARE(bar.keyTipLevel(), 0);
        // 第 0 層：兩個分頁 + 一個快速存取項目。
        QCOMPARE(bar.visibleKeyTips(), QStringList({"H", "V", "1"}));

        QVERIFY(bar.handleKeyTip(u'V'));
        QCOMPARE(bar.keyTipLevel(), 1);
        QCOMPARE(bar.currentPageId(), QStringLiteral("view"));
        QCOMPARE(bar.visibleKeyTips(), QStringList({"I", "O"}));

        QSignalSpy relayed(&bar, &RibbonBar::actionTriggered);
        QVERIFY(bar.handleKeyTip(u'o'));  // 大小寫皆可
        QCOMPARE(relayed.count(), 1);
        QCOMPARE(relayed.takeFirst().at(0).toString(), QStringLiteral("view.zoomOut"));
        QVERIFY(!bar.keyTipsVisible());

        // 沒顯示提示時的字元不該有任何作用。
        QVERIFY(!bar.handleKeyTip(u'H'));
        bar.showKeyTips();
        QVERIFY(!bar.handleKeyTip(u'Z'));
        bar.hideKeyTips();
        QVERIFY(!bar.keyTipsVisible());
    }

    void altKeyDrivesKeyTipsFromKeyboard() {
        // 上一個測試走的是 API，這個測試走真的鍵盤事件：兩條路徑若不一致，
        // 就會出現「單元測試全綠但按 Alt 沒反應」這種最難查的問題。
        ActionRegistry registry;
        QObject owner;
        const Layout layout = twoPageLayout();
        injectActions(registry, layout, &owner);

        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);
        bar.show();
        QTest::qWait(50);

        QTest::keyClick(&bar, Qt::Key_Alt);
        QVERIFY(bar.keyTipsVisible());
        QCOMPARE(bar.keyTipLevel(), 0);

        QTest::keyClick(&bar, Qt::Key_V);
        QCOMPARE(bar.keyTipLevel(), 1);
        QCOMPARE(bar.currentPageId(), QStringLiteral("view"));

        // Esc 回上一層而不是整個關掉，否則按錯一個字母就得重按 Alt。
        QTest::keyClick(&bar, Qt::Key_Escape);
        QCOMPARE(bar.keyTipLevel(), 0);
        QVERIFY(bar.keyTipsVisible());

        QTest::keyClick(&bar, Qt::Key_Escape);
        QVERIFY(!bar.keyTipsVisible());

        // 再按一次 Alt 開啟，第二次 Alt 關閉。
        QTest::keyClick(&bar, Qt::Key_Alt);
        QVERIFY(bar.keyTipsVisible());
        QTest::keyClick(&bar, Qt::Key_Alt);
        QVERIFY(!bar.keyTipsVisible());
    }

    void emptyLayoutDoesNotCrash() {
        ActionRegistry registry;
        RibbonBar bar(&registry);
        bar.setLayoutModel(Layout{});
        bar.show();
        QTest::qWait(50);

        QCOMPARE(bar.pageCount(), 0);
        QCOMPARE(bar.currentPageId(), QString{});
        QVERIFY(bar.page(0) == nullptr);
        QVERIFY(bar.buttonForActionId(QStringLiteral("edit.copy")) == nullptr);
        bar.setCurrentPageIndex(3);
        bar.setCollapsed(true);
        bar.showKeyTips();
        QVERIFY(!bar.handleKeyTip(u'H'));
        bar.hideKeyTips();

        // 換成有內容的配置後要能正常運作，反之亦然。
        bar.setLayoutModel(twoPageLayout());
        QCOMPARE(bar.pageCount(), 2);
        bar.setLayoutModel(Layout{});
        QCOMPARE(bar.pageCount(), 0);
    }

    void malformedConfigurationStillBuilds() {
        // 使用者手改壞的設定檔經過 fromJson 之後，Ribbon 必須照樣建得起來。
        const QJsonObject broken =
            QJsonDocument::fromJson(R"({"pages":[{"id":"x","groups":[{"id":"g","items":[
                {"actionId":"unknown.action"},{"type":"separator"},{"label":"沒有 id"}]}]}]})")
                .object();
        ParseResult result;
        const Layout layout = fromJson(broken, &result);
        QVERIFY(result.ok());

        ActionRegistry registry;
        RibbonBar bar(&registry);
        bar.setLayoutModel(layout);
        bar.show();
        QTest::qWait(50);

        QCOMPARE(bar.pageCount(), 1);
        QCOMPARE(bar.page(0)->itemCount(), 1);
        QCOMPARE(bar.missingActionIds(), QStringList{QStringLiteral("unknown.action")});
        // 分頁沒有 title 時退回以 id 當標籤，不留空白分頁。
        QCOMPARE(bar.tabBar()->tabText(0), QStringLiteral("x"));
    }

    void registryDropsDestroyedActions() {
        ActionRegistry registry;
        RibbonBar bar(&registry);
        bar.setLayoutModel(twoPageLayout());

        auto* temporary = new QAction(QStringLiteral("暫時"));
        registry.registerAction(QStringLiteral("edit.copy"), temporary);
        RibbonButton* copy = bar.buttonForActionId(QStringLiteral("edit.copy"));
        QVERIFY(!copy->isPlaceholder());

        // 動作隨其擁有者消失（例如文件關閉）後，按鈕要退回佔位而不是指向已刪除的物件。
        delete temporary;
        QVERIFY(!registry.contains(QStringLiteral("edit.copy")));
        QVERIFY(copy->isPlaceholder());
        copy->click();
    }

    // 觸控目標（PRD-UI-013）。
    //
    // 這一條先前只有換算函式有測試，「Ribbon 按鈕是否真的達到門檻」沒有——
    // 而那正是使用者會碰到的那一半。用手指按不準的按鈕不會有錯誤訊息，
    // 只會讓人一直按錯隔壁那顆。
    void touchModeEnlargesEveryButtonToTheMinimumTarget() {
        ActionRegistry registry;
        RibbonBar bar(&registry);
        bar.setLayoutModel(defaultLayout());

        const auto minimumForBar = [&bar] {
            const double dpi = bar.logicalDpiX() > 0 ? static_cast<double>(bar.logicalDpiX())
                                                     : alioth::app::TouchTargetMetrics::kReferenceDpi;
            return alioth::app::TouchTargetMetrics::minimumTargetSizePx(dpi);
        };

        const QList<RibbonButton*> buttons = bar.findChildren<RibbonButton*>();
        QVERIFY(!buttons.isEmpty());

        bar.setTouchMode(true);
        QVERIFY(bar.touchMode());
        for (RibbonButton* button : buttons) {
            const QSize hint = button->sizeHint();
            QVERIFY2(alioth::app::TouchTargetMetrics::meetsMinimumTarget(
                         QSizeF(hint), bar.logicalDpiX()),
                     qPrintable(QStringLiteral("%1 的觸控目標只有 %2x%3，門檻是 %4")
                                    .arg(button->actionId())
                                    .arg(hint.width())
                                    .arg(hint.height())
                                    .arg(minimumForBar())));
        }

        // 換配置之後造出來的是全新的按鈕。忘了重新套上去的話，整條 Ribbon
        // 會靜靜地退回滑鼠尺寸，而設定畫面上的開關看起來還是開著的。
        bar.setLayoutModel(defaultLayout());
        for (RibbonButton* button : bar.findChildren<RibbonButton*>()) {
            QVERIFY2(button->touchMode(), "換配置之後觸控模式掉了");
        }

        // 關掉要真的縮回去，否則這個開關等於單向的。
        bar.setTouchMode(false);
        bool anySmaller = false;
        for (RibbonButton* button : bar.findChildren<RibbonButton*>()) {
            if (!alioth::app::TouchTargetMetrics::meetsMinimumTarget(QSizeF(button->sizeHint()),
                                                                    bar.logicalDpiX())) {
                anySmaller = true;
                break;
            }
        }
        QVERIFY2(anySmaller, "關掉觸控模式之後沒有任何按鈕縮回去——開關可能沒有作用");
    }
};

QTEST_MAIN(TestRibbonBar)
#include "test_ribbon_bar.moc"
