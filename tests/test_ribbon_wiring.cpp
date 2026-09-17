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

#include "ui/main_window.h"
#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_default_layout.h"
#include "ui/ribbon/ribbon_model.h"

using namespace alioth;

namespace {

// 目前允許的缺口上限。**只能往下調，不能往上**。
//
// 這個數字本身沒有意義，它的作用是棘輪：接上更多動作之後把它調小，
// 之後任何一次把按鈕加進配置卻忘了註冊動作的改動就會紅。
constexpr int kMaxUnwiredActions = 14;  // 2026-09-17：page.resize 接上（PRD-PAGE-003）

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

QTEST_MAIN(TestRibbonWiring)
#include "test_ribbon_wiring.moc"
