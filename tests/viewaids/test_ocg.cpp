// PRD-VIEW-008：圖層面板領域模型（互斥群組、鎖定）。
// 解析 /OCProperties 位元組的測試在 test_ocg_reader.cpp；這裡只測面板互動邏輯。

#include <QtTest>

#include "domain/ocg.h"

using namespace alioth::domain;

class TestOcg : public QObject {
    Q_OBJECT

private slots:
    void toggleSimpleLayer() {
        OcgTree tree;
        tree.layers.push_back(OcgLayer{1, "A", true, false, false, -1, {}});
        tree.roots.push_back(0);

        const auto changed = setLayerVisible(tree, 0, false);
        QCOMPARE(changed.size(), std::size_t(1));
        QVERIFY(!tree.layers[0].visible);
    }

    void radioGroupMutualExclusion() {
        OcgTree tree;
        tree.layers.push_back(OcgLayer{1, "A", true, false, false, 0, {}});
        tree.layers.push_back(OcgLayer{2, "B", false, false, false, 0, {}});
        tree.layers.push_back(OcgLayer{3, "C", false, false, false, 0, {}});
        tree.roots = {0, 1, 2};

        // 打開 B：同群組的 A 應該自動關閉，C 本來就是關的不算「改動」。
        const auto changed = setLayerVisible(tree, 1, true);
        QVERIFY(tree.layers[1].visible);
        QVERIFY(!tree.layers[0].visible);
        QVERIFY(!tree.layers[2].visible);
        QCOMPARE(changed.size(), std::size_t(2));  // B 打開 + A 關閉
    }

    void lockedLayerIgnoresToggle() {
        OcgTree tree;
        tree.layers.push_back(OcgLayer{1, "A", true, true, false, -1, {}});
        const auto changed = setLayerVisible(tree, 0, false);
        QVERIFY(changed.empty());
        QVERIFY(tree.layers[0].visible);  // 未被改動
    }

    void groupHeadingIsNotToggleable() {
        OcgTree tree;
        OcgLayer heading;
        heading.objectNumber = -1;
        heading.isGroupHeading = true;
        heading.visible = true;
        tree.layers.push_back(heading);
        const auto changed = setLayerVisible(tree, 0, false);
        QVERIFY(changed.empty());
    }

    // PRD-VIEW-014：OCMD 的 /P 政策合併（純布林邏輯，不牽涉 PDF 剖析）。
    void ocmdPolicyAnyOn() {
        QVERIFY(resolveOcmdPolicy(OcmdPolicy::AnyOn, {false, true, false}));
        QVERIFY(!resolveOcmdPolicy(OcmdPolicy::AnyOn, {false, false}));
    }

    void ocmdPolicyAllOn() {
        QVERIFY(resolveOcmdPolicy(OcmdPolicy::AllOn, {true, true}));
        QVERIFY(!resolveOcmdPolicy(OcmdPolicy::AllOn, {true, false}));
    }

    void ocmdPolicyAnyOff() {
        QVERIFY(resolveOcmdPolicy(OcmdPolicy::AnyOff, {true, false}));
        QVERIFY(!resolveOcmdPolicy(OcmdPolicy::AnyOff, {true, true}));
    }

    void ocmdPolicyAllOff() {
        QVERIFY(resolveOcmdPolicy(OcmdPolicy::AllOff, {false, false}));
        QVERIFY(!resolveOcmdPolicy(OcmdPolicy::AllOff, {false, true}));
    }

    void ocmdPolicyEmptyMembersIsConservativelyVisible() {
        QVERIFY(resolveOcmdPolicy(OcmdPolicy::AnyOn, {}));
        QVERIFY(resolveOcmdPolicy(OcmdPolicy::AllOff, {}));
    }

    void findByObjectNumber() {
        OcgTree tree;
        tree.layers.push_back(OcgLayer{42, "A", true, false, false, -1, {}});
        QVERIFY(tree.find(42) != nullptr);
        QCOMPARE(QString::fromStdString(tree.find(42)->name), QStringLiteral("A"));
        QVERIFY(tree.find(99) == nullptr);
    }
};

QTEST_MAIN(TestOcg)
#include "test_ocg.moc"
