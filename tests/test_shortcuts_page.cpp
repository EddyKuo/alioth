// 快捷鍵設定頁（PRD-UI-004）。
//
// 鍵位規則本身在 tests/uisystem/test_shortcut_scheme.cpp 測過了（無 GUI）。
// 這一支只驗那層薄殼有沒有把模型接對，而接錯的三種方式都不會有錯誤訊息：
//
//   1. 衝突被靜默套用 → 被搶走鍵位的功能變成「按了沒反應」，查不出原因。
//   2. 「清除」變成「設成空字串再判成衝突」→ 使用者移不掉一個鍵。
//   3. 改完不發訊號 → 畫面上的表更新了，執行中的 QAction 沒有，
//      而且要等下次重啟才會露餡。

#include <QtTest>

#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTreeWidget>

#include "app/uisystem/shortcut_scheme.h"
#include "ui/shortcuts_page.h"

using namespace alioth;

class TestShortcutsPage : public QObject {
    Q_OBJECT

private slots:
    void listsEveryBindingInTheScheme();
    void conflictIsReportedAndNotApplied();
    void clearingRemovesTheKeyAndNotifies();
    void applyingAFreeKeyUpdatesTheRowAndNotifies();

private:
    static QTreeWidgetItem* selectAction(QTreeWidget* tree, const QString& actionId) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (item->data(0, Qt::UserRole + 1).toString() == actionId) {
                tree->setCurrentItem(item);
                return item;
            }
        }
        return nullptr;
    }
};

void TestShortcutsPage::listsEveryBindingInTheScheme() {
    app::ShortcutScheme scheme;
    ui::ShortcutsPage page(&scheme);
    auto* tree = page.findChild<QTreeWidget*>(QStringLiteral("shortcutsTree"));
    QVERIFY(tree != nullptr);
    // 少列一條就等於那個動作的鍵位改不到，而畫面上完全看不出少了什麼。
    QCOMPARE(tree->topLevelItemCount(), static_cast<int>(scheme.bindings().size()));
    QVERIFY(tree->topLevelItemCount() > 0);
}

void TestShortcutsPage::conflictIsReportedAndNotApplied() {
    app::ShortcutScheme scheme;
    ui::ShortcutsPage page(&scheme);
    QSignalSpy changed(&page, &ui::ShortcutsPage::schemeChanged);

    auto* tree = page.findChild<QTreeWidget*>(QStringLiteral("shortcutsTree"));
    auto* editor = page.findChild<QKeySequenceEdit*>(QStringLiteral("shortcutsEditor"));
    auto* apply = page.findChild<QPushButton*>(QStringLiteral("shortcutsApply"));
    auto* status = page.findChild<QLabel*>(QStringLiteral("shortcutsStatus"));
    QVERIFY(tree != nullptr && editor != nullptr && apply != nullptr && status != nullptr);

    // 找兩個同情境、都有鍵位的動作，拿其中一個的鍵去搶另一個。
    QString victimId;
    QString attackerId;
    QKeySequence taken;
    for (const app::ShortcutBinding& a : scheme.bindings()) {
        if (a.isEmpty()) continue;
        for (const app::ShortcutBinding& b : scheme.bindings()) {
            if (b.actionId == a.actionId || b.context != a.context) continue;
            victimId = a.actionId;
            taken = a.sequence;
            attackerId = b.actionId;
            break;
        }
        if (!victimId.isEmpty()) break;
    }
    QVERIFY2(!victimId.isEmpty(), "預設鍵位表裡找不到兩個同情境的動作");

    QVERIFY(selectAction(tree, attackerId) != nullptr);
    editor->setKeySequence(taken);
    apply->click();

    // 未套用，而且說得出原因。
    QCOMPARE(scheme.binding(victimId)->sequence, taken);
    QVERIFY2(scheme.binding(attackerId)->sequence != taken, "衝突的鍵位被套用了");
    QVERIFY(!status->text().isEmpty());
    QCOMPARE(changed.count(), 0);
}

void TestShortcutsPage::clearingRemovesTheKeyAndNotifies() {
    app::ShortcutScheme scheme;
    ui::ShortcutsPage page(&scheme);
    QSignalSpy changed(&page, &ui::ShortcutsPage::schemeChanged);

    auto* tree = page.findChild<QTreeWidget*>(QStringLiteral("shortcutsTree"));
    auto* clear = page.findChild<QPushButton*>(QStringLiteral("shortcutsClear"));
    QVERIFY(tree != nullptr && clear != nullptr);

    QString target;
    for (const app::ShortcutBinding& binding : scheme.bindings()) {
        if (!binding.isEmpty()) {
            target = binding.actionId;
            break;
        }
    }
    QVERIFY(!target.isEmpty());

    QVERIFY(selectAction(tree, target) != nullptr);
    clear->click();

    // 移除快捷鍵是使用者的權利，空鍵不與任何鍵衝突，所以一定成功。
    QVERIFY2(scheme.binding(target)->isEmpty(), "清除沒有真的移除鍵位");
    QCOMPARE(changed.count(), 1);
}

void TestShortcutsPage::applyingAFreeKeyUpdatesTheRowAndNotifies() {
    app::ShortcutScheme scheme;
    ui::ShortcutsPage page(&scheme);
    QSignalSpy changed(&page, &ui::ShortcutsPage::schemeChanged);

    auto* tree = page.findChild<QTreeWidget*>(QStringLiteral("shortcutsTree"));
    auto* editor = page.findChild<QKeySequenceEdit*>(QStringLiteral("shortcutsEditor"));
    auto* apply = page.findChild<QPushButton*>(QStringLiteral("shortcutsApply"));
    QVERIFY(tree != nullptr && editor != nullptr && apply != nullptr);

    const QString target = scheme.bindings().front().actionId;
    QTreeWidgetItem* row = selectAction(tree, target);
    QVERIFY(row != nullptr);

    // 找一個沒人用的鍵。F13 之後的功能鍵在預設表裡不會出現。
    const QKeySequence free(QStringLiteral("Ctrl+Alt+Shift+F13"));
    editor->setKeySequence(free);
    apply->click();

    QCOMPARE(scheme.binding(target)->sequence, free);
    QCOMPARE(changed.count(), 1);

    // 表也要跟著更新——模型改了而畫面沒改，使用者會以為沒生效而再按一次。
    QTreeWidgetItem* refreshed = selectAction(tree, target);
    QVERIFY(refreshed != nullptr);
    QCOMPARE(refreshed->text(2), free.toString(QKeySequence::NativeText));
}

QTEST_MAIN(TestShortcutsPage)
#include "test_shortcuts_page.moc"
