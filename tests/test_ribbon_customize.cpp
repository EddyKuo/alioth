// 自訂 Ribbon 對話框的測試（PRD-UI-010 / PRD-UI-011）。
//
// 對話框改的是 ribbon::Layout 這份資料，所以驗證的方式是操作 widget、收下
// result()，再比對資料。不去比對畫面上的按鈕：Ribbon 怎麼把資料畫出來已經由
// tests/ribbon/test_ribbon_bar.cpp 守著，這裡重複一次只會讓兩邊一起壞掉時
// 分不出是誰的責任。

#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>

#include "ui/ribbon/action_registry.h"
#include "ui/ribbon/ribbon_model.h"
#include "ui/ribbon_customize_dialog.h"

using namespace alioth::ui;

namespace {

[[nodiscard]] ribbon::Layout sampleLayout() {
    ribbon::Item copy;
    copy.actionId = QStringLiteral("edit.copy");
    copy.iconName = QStringLiteral("edit.copy");
    copy.keyTip = QStringLiteral("C");
    copy.size = ribbon::ItemSize::Large;

    ribbon::Group clipboard;
    clipboard.id = QStringLiteral("home.clipboard");
    clipboard.title = QStringLiteral("剪貼簿");
    clipboard.keyTip = QStringLiteral("K");
    clipboard.items = {copy};

    ribbon::Page home;
    home.id = QStringLiteral("home");
    home.title = QStringLiteral("常用");
    home.keyTip = QStringLiteral("H");
    home.groups = {clipboard};

    ribbon::Layout layout;
    layout.pages = {home};
    layout.quickAccessActionIds = {QStringLiteral("edit.copy")};
    return layout;
}

// 註冊三個假動作，讓「可用的功能」清單有東西可挑。
void fillRegistry(ribbon::ActionRegistry* registry, QObject* owner) {
    for (const QString& id : {QStringLiteral("edit.copy"), QStringLiteral("edit.paste"),
                              QStringLiteral("view.zoomIn")}) {
        auto* action = new QAction(id, owner);
        registry->registerAction(id, action);
    }
}

[[nodiscard]] int rowOf(QListWidget* list, const QString& actionId) {
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->toolTip() == actionId) return i;
    }
    return -1;
}

}  // namespace

class TestRibbonCustomize : public QObject {
    Q_OBJECT

private slots:
    // 什麼都不動就確定，資料必須原封不動。這是整個對話框最重要的性質：
    // 開起來看一眼再關掉，不該把使用者的配置改成別的樣子。
    void roundTripsUnchangedLayout();
    void addsActionToSelectedGroup();
    void removesItemButNotGroup();
    void reordersItems();
    void editsQuickAccess();
    void cancelKeepsOriginalLayout();
};

void TestRibbonCustomize::roundTripsUnchangedLayout() {
    ribbon::ActionRegistry registry;
    fillRegistry(&registry, &registry);

    const ribbon::Layout original = sampleLayout();
    RibbonCustomizeDialog dialog(original, &registry);
    auto* buttons = dialog.findChild<QDialogButtonBox*>(QStringLiteral("ribbonCustomizeButtons"));
    QVERIFY(buttons != nullptr);
    buttons->button(QDialogButtonBox::Ok)->click();

    QCOMPARE(dialog.result(), original);
}

void TestRibbonCustomize::addsActionToSelectedGroup() {
    ribbon::ActionRegistry registry;
    fillRegistry(&registry, &registry);

    RibbonCustomizeDialog dialog(sampleLayout(), &registry);
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("ribbonStructure"));
    auto* available = dialog.findChild<QListWidget*>(QStringLiteral("ribbonAvailableActions"));
    QVERIFY(tree != nullptr);
    QVERIFY(available != nullptr);

    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    const int row = rowOf(available, QStringLiteral("view.zoomIn"));
    QVERIFY(row >= 0);
    available->setCurrentRow(row);

    dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeAdd"))->click();
    dialog.findChild<QDialogButtonBox*>(QStringLiteral("ribbonCustomizeButtons"))
        ->button(QDialogButtonBox::Ok)
        ->click();

    const ribbon::Layout result = dialog.result();
    const QList<ribbon::Item>& items = result.pages.at(0).groups.at(0).items;
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(1).actionId, QStringLiteral("view.zoomIn"));
    // 原本就在的那顆必須整份留著——keyTip 與 iconName 在樹上都沒有呈現，
    // 若是從樹重建就會被清成空字串。
    QCOMPARE(items.at(0).keyTip, QStringLiteral("C"));
    QCOMPARE(items.at(0).iconName, QStringLiteral("edit.copy"));
    QCOMPARE(result.pages.at(0).keyTip, QStringLiteral("H"));
    QCOMPARE(result.pages.at(0).groups.at(0).keyTip, QStringLiteral("K"));
}

void TestRibbonCustomize::removesItemButNotGroup() {
    ribbon::ActionRegistry registry;
    fillRegistry(&registry, &registry);

    RibbonCustomizeDialog dialog(sampleLayout(), &registry);
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("ribbonStructure"));
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeRemove"));

    // 選在群組上按移除：不該有任何事發生，也不該當掉。
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    QVERIFY(!remove->isEnabled());
    remove->click();
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);

    tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
    QVERIFY(remove->isEnabled());
    remove->click();

    dialog.findChild<QDialogButtonBox*>(QStringLiteral("ribbonCustomizeButtons"))
        ->button(QDialogButtonBox::Ok)
        ->click();

    const ribbon::Layout result = dialog.result();
    QCOMPARE(result.pages.size(), 1);
    QCOMPARE(result.pages.at(0).groups.size(), 1);
    QVERIFY(result.pages.at(0).groups.at(0).items.isEmpty());
}

void TestRibbonCustomize::reordersItems() {
    ribbon::ActionRegistry registry;
    fillRegistry(&registry, &registry);

    RibbonCustomizeDialog dialog(sampleLayout(), &registry);
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("ribbonStructure"));
    auto* available = dialog.findChild<QListWidget*>(QStringLiteral("ribbonAvailableActions"));

    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    available->setCurrentRow(rowOf(available, QStringLiteral("edit.paste")));
    dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeAdd"))->click();

    QTreeWidgetItem* group = tree->topLevelItem(0)->child(0);
    QCOMPARE(group->childCount(), 2);
    tree->setCurrentItem(group->child(1));
    dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeUp"))->click();

    // 已經在最上面，再往上不該把它搬到別的群組或消失。
    dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeUp"))->click();

    dialog.findChild<QDialogButtonBox*>(QStringLiteral("ribbonCustomizeButtons"))
        ->button(QDialogButtonBox::Ok)
        ->click();

    const QList<ribbon::Item>& items = dialog.result().pages.at(0).groups.at(0).items;
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(0).actionId, QStringLiteral("edit.paste"));
    QCOMPARE(items.at(1).actionId, QStringLiteral("edit.copy"));
}

void TestRibbonCustomize::editsQuickAccess() {
    ribbon::ActionRegistry registry;
    fillRegistry(&registry, &registry);

    RibbonCustomizeDialog dialog(sampleLayout(), &registry);
    auto* available = dialog.findChild<QListWidget*>(QStringLiteral("ribbonAvailableActions"));
    auto* quick = dialog.findChild<QListWidget*>(QStringLiteral("ribbonQuickAccess"));
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeAddQuickAccess"));

    available->setCurrentRow(rowOf(available, QStringLiteral("view.zoomIn")));
    add->click();
    // 同一顆加兩次只會讓工具列出現兩顆一樣的按鈕，必須被擋掉。
    add->click();
    QCOMPARE(quick->count(), 2);

    quick->setCurrentRow(rowOf(quick, QStringLiteral("edit.copy")));
    dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeRemoveQuickAccess"))->click();

    dialog.findChild<QDialogButtonBox*>(QStringLiteral("ribbonCustomizeButtons"))
        ->button(QDialogButtonBox::Ok)
        ->click();

    QCOMPARE(dialog.result().quickAccessActionIds, QStringList{QStringLiteral("view.zoomIn")});
}

void TestRibbonCustomize::cancelKeepsOriginalLayout() {
    ribbon::ActionRegistry registry;
    fillRegistry(&registry, &registry);

    const ribbon::Layout original = sampleLayout();
    RibbonCustomizeDialog dialog(original, &registry);
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("ribbonStructure"));
    tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
    dialog.findChild<QPushButton*>(QStringLiteral("ribbonCustomizeRemove"))->click();

    dialog.findChild<QDialogButtonBox*>(QStringLiteral("ribbonCustomizeButtons"))
        ->button(QDialogButtonBox::Cancel)
        ->click();

    // 取消時 collectFromTree() 沒被呼叫過，result() 仍是進來時的那份。
    QCOMPARE(dialog.result(), original);
}

QTEST_MAIN(TestRibbonCustomize)
#include "test_ribbon_customize.moc"
