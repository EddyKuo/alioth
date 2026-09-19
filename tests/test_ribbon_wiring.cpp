// Ribbon 配置與實際動作的勾稽（PRD-UI-002）。
//
// Ribbon 只認字串 id，真正的 QAction 由 MainWindow 注入。這道間接層讓 Ribbon
// 可以獨立測試，代價是「配置裡寫了一個沒人註冊的 id」不會有任何錯誤——那顆
// 按鈕只是永遠停用，看起來像壞掉。
//
// 這支測試把預設配置與 MainWindow 真正註冊的動作對起來，讓缺口變成一個
// 具體的數字而不是使用者的抱怨。門檻寫成「不得比現況更差」：一次補完 107 顆
// 按鈕不現實，但每次改動都不該讓缺口變大。

#include <QtTest>

#include <QAction>
#include <QSettings>

#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QToolButton>

#include <functional>

#include "ui/main_window.h"
#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_button.h"
#include "ui/ribbon/ribbon_default_layout.h"
#include "ui/ribbon/ribbon_model.h"

using namespace alioth;

namespace {

// 目前允許的缺口上限。**只能往下調，不能往上**。
//
// 這個數字本身沒有意義，它的作用是棘輪：接上更多動作之後把它調小，
// 之後任何一次把按鈕加進配置卻忘了註冊動作的改動就會紅。
// 2026-09-19：歸零。剩下那 14 顆分成兩批處理——工作階段、區域縮放、
// 安全性與權限、移除密碼補上實作；認證文件、時間戳、Tab 順序、背景、
// 說明主題／版本資訊／檢查更新／回報問題沒有實作，從預設配置拿掉。
// 灰色按鈕對使用者的意思是「壞了」，不是「還沒做」。
constexpr int kMaxUnwiredActions = 0;

}  // namespace

class TestRibbonWiring : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // 換掉 QSettings 範圍：MainWindow 會讀使用者自訂的 ribbon.json 與版面，
        // 開發機上的殘留設定會讓這支測試量到別的東西。
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("RibbonWiring"));
        QSettings().clear();
    }

    void defaultLayoutReferencesOnlyRegisteredActions();
    void everyRegisteredActionIdIsUnique();
    void menuActionsInTheLayoutActuallyOpenTheirMenu();
    void moveAndNUpAreSeparateActions();
    void annotationActionsAreReachableFromTheClassicMenuBar();
};

void TestRibbonWiring::defaultLayoutReferencesOnlyRegisteredActions() {
    ui::MainWindow window;

    // 從視窗上找出 ActionRegistry：它是 MainWindow 的子物件，
    // 不必為了測試把它變成公開介面。
    auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
    QVERIFY2(registry != nullptr, "MainWindow 沒有 ActionRegistry");

    const QStringList required = ui::ribbon::collectActionIds(ui::ribbon::defaultLayout());
    QVERIFY(!required.isEmpty());
    const QStringList missing = registry->missingIds(required);

    if (!missing.isEmpty()) {
        qInfo() << "Ribbon 尚未接上的動作 (" << missing.size() << "/" << required.size()
                << "):" << missing;
    }
    QVERIFY2(missing.size() <= kMaxUnwiredActions,
             qPrintable(QStringLiteral("Ribbon 未接上的動作從 %1 增加到 %2——"
                                       "把按鈕加進預設配置時要一併註冊動作，"
                                       "否則那顆按鈕永遠停用，看起來像壞掉")
                            .arg(kMaxUnwiredActions)
                            .arg(missing.size())));
}

void TestRibbonWiring::everyRegisteredActionIdIsUnique() {
    ui::MainWindow window;
    auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
    QVERIFY(registry != nullptr);

    // registerAction 允許覆寫（分階段注入需要），代價是打錯字的 id 會靜靜
    // 蓋掉另一個動作。ids() 本身是去重的，所以這裡驗的是「註冊過的都還在」。
    const QStringList ids = registry->ids();
    QCOMPARE(ids.size(), registry->count());
    for (const QString& id : ids) {
        QVERIFY2(registry->action(id) != nullptr,
                 qPrintable(QStringLiteral("%1 註冊過但取不回 QAction——"
                                           "多半是擁有者已經被刪掉").arg(id)));
    }
}

// 指向子選單的動作在 Ribbon 上必須真的彈出選單。
//
// QMenu::menuAction() 註冊起來一切正常，但 Ribbon 按鈕的點擊只做
// action->trigger()，而觸發一個選單動作只會發出 triggered——彈出是
// QMenuBar 或 QToolButton::setMenu 的行為。少了這一段，「最近使用」
// 「管理設定」「設定狀態」在 Ribbon 上按了完全沒反應，而在傳統選單裡
// 一切正常，於是這個缺陷只在預設介面上出現。
void TestRibbonWiring::menuActionsInTheLayoutActuallyOpenTheirMenu() {
    ui::MainWindow window;
    auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
    QVERIFY(registry != nullptr);

    const QStringList inLayout = ui::ribbon::collectActionIds(ui::ribbon::defaultLayout());
    QStringList menuIds;
    for (const QString& id : inLayout) {
        const QAction* action = registry->action(id);
        if (action != nullptr && action->menu() != nullptr) menuIds << id;
    }
    QVERIFY2(!menuIds.isEmpty(), "預設配置裡沒有任何選單型動作，這條測不到");

    // Ribbon 的按鈕是 QToolButton。掛了選單的那些必須設成 InstantPopup，
    // 否則按下去只會 trigger 一個什麼都不做的 menuAction。
    const QList<ui::ribbon::RibbonButton*> buttons =
        window.findChildren<ui::ribbon::RibbonButton*>();
    QVERIFY2(!buttons.isEmpty(), "找不到任何 Ribbon 按鈕");

    for (const QString& id : menuIds) {
        bool checked = false;
        for (ui::ribbon::RibbonButton* button : buttons) {
            if (button->actionId() != id) continue;
            checked = true;
            QVERIFY2(button->menu() != nullptr,
                     qPrintable(QStringLiteral("%1 指向一個子選單，但按鈕上沒有掛選單——"
                                               "按下去不會有任何反應").arg(id)));
            QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
        }
        QVERIFY2(checked, qPrintable(QStringLiteral("%1 在配置裡卻找不到對應的按鈕").arg(id)));
    }
}

// 「移動頁面」與「N 頁併一頁」是兩個動作。
//
// 先前 page.move 註冊的是 N 頁併一頁：使用者按 Ribbon 上的「移動」會被問
// 「每張要放幾頁」，確認之後前 N 頁被合成一頁——而那是一次全檔重寫。
void TestRibbonWiring::moveAndNUpAreSeparateActions() {
    ui::MainWindow window;
    auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
    QVERIFY(registry != nullptr);

    const QAction* move = registry->action(QStringLiteral("page.move"));
    const QAction* nUp = registry->action(QStringLiteral("page.nUp"));
    QVERIFY2(move != nullptr, "page.move 沒有註冊");
    QVERIFY2(nUp != nullptr, "page.nUp 沒有註冊");
    QVERIFY2(move != nUp, "page.move 與 page.nUp 指到同一個動作");
    QVERIFY2(move->text().contains(QStringLiteral("移動")),
             qPrintable(QStringLiteral("page.move 的文字是「%1」").arg(move->text())));
}

// 傳統選單列（view.classicMenu）與 Ribbon 是互斥顯示的兩套入口，功能不齊
// 等於那條路走不通。
//
// 刪除註解、回覆、設定狀態、複製註解先前只掛在註解清單與 Ribbon 上：
// 它們的 parent 必須是清單（快捷鍵才不會與頁面上的 Delete 互搶），而清單在
// buildDockPanels() 才建得出來，那時 buildActions() 的選單已經封好了。
// 結果是切到傳統選單的使用者在選單列上完全找不到刪除註解。
void TestRibbonWiring::annotationActionsAreReachableFromTheClassicMenuBar() {
    ui::MainWindow window;
    auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
    QVERIFY(registry != nullptr);

    // 選單可以巢狀（「設定狀態」本身是子選單），所以要遞迴展開。
    QSet<const QAction*> reachable;
    std::function<void(const QMenu*)> collect = [&](const QMenu* menu) {
        if (menu == nullptr) return;
        for (const QAction* action : menu->actions()) {
            if (reachable.contains(action)) continue;
            reachable.insert(action);
            collect(action->menu());
        }
    };
    for (const QAction* top : window.menuBar()->actions()) {
        reachable.insert(top);
        collect(top->menu());
    }

    for (const char* id : {"comment.reply", "comment.setStatus", "comment.delete",
                           "comment.copy"}) {
        const QAction* action = registry->action(QString::fromLatin1(id));
        QVERIFY2(action != nullptr, qPrintable(QStringLiteral("%1 沒有註冊").arg(id)));
        QVERIFY2(reachable.contains(action),
                 qPrintable(QStringLiteral("%1 在傳統選單列上找不到").arg(id)));
    }
}

QTEST_MAIN(TestRibbonWiring)
#include "test_ribbon_wiring.moc"
