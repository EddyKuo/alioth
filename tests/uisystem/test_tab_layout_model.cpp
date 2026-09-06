// 多頁籤分離／合併的狀態轉移測試（PRD-UI-001）。

#include <QtTest>

#include "app/uisystem/tab_layout_model.h"

using namespace alioth::app;

class TestTabLayoutModel : public QObject {
    Q_OBJECT

private slots:
    void startsWithEmptyPrimaryWindow() {
        TabLayoutModel model;
        QCOMPARE(model.windows().size(), std::size_t(1));
        const auto* primary = model.window(model.primaryWindowId());
        QVERIFY(primary != nullptr);
        QVERIFY(primary->tabs.empty());
    }

    void openTabAddsToTargetWindow() {
        TabLayoutModel model;
        const auto result = model.openTab({QStringLiteral("doc-a"), QStringLiteral("A.pdf")});
        QVERIFY(result.ok);
        const auto* primary = model.window(model.primaryWindowId());
        QCOMPARE(primary->tabs.size(), std::size_t(1));
        QCOMPARE(primary->activeIndex, 0);

        const auto loc = model.locate(QStringLiteral("doc-a"));
        QVERIFY(loc.has_value());
        QCOMPARE(loc->first, model.primaryWindowId());
        QCOMPARE(loc->second, 0);
    }

    void detachTabCreatesNewWindow() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        model.openTab({QStringLiteral("doc-b"), QStringLiteral("B")});

        const auto result = model.detachTab(model.primaryWindowId(), 0);
        QVERIFY(result.ok);
        QVERIFY(result.createdWindowId.has_value());

        const auto* primary = model.window(model.primaryWindowId());
        QCOMPARE(primary->tabs.size(), std::size_t(1));
        QCOMPARE(primary->tabs[0].documentId, QStringLiteral("doc-b"));

        const auto* detached = model.window(*result.createdWindowId);
        QVERIFY(detached != nullptr);
        QCOMPARE(detached->tabs.size(), std::size_t(1));
        QCOMPARE(detached->tabs[0].documentId, QStringLiteral("doc-a"));
        QCOMPARE(model.windows().size(), std::size_t(2));
    }

    void detachSingleTabWindowIsRejected() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        const auto result = model.detachTab(model.primaryWindowId(), 0);
        QVERIFY(!result.ok);
        QCOMPARE(model.windows().size(), std::size_t(1));
    }

    void mergeWindowBringsTabsBackAndClosesSource() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        model.openTab({QStringLiteral("doc-b"), QStringLiteral("B")});
        const auto detach = model.detachTab(model.primaryWindowId(), 0);
        const WindowId detachedId = *detach.createdWindowId;

        const auto merge = model.mergeWindow(detachedId, model.primaryWindowId());
        QVERIFY(merge.ok);
        QCOMPARE(merge.closedWindowId, std::optional<WindowId>(detachedId));
        QVERIFY(model.window(detachedId) == nullptr);

        const auto* primary = model.window(model.primaryWindowId());
        QCOMPARE(primary->tabs.size(), std::size_t(2));
        QVERIFY(model.locate(QStringLiteral("doc-a")).has_value());
        QVERIFY(model.locate(QStringLiteral("doc-b")).has_value());
    }

    void mergeCannotUsePrimaryAsSource() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        model.openTab({QStringLiteral("doc-b"), QStringLiteral("B")});
        const auto detach = model.detachTab(model.primaryWindowId(), 0);

        const auto merge = model.mergeWindow(model.primaryWindowId(), *detach.createdWindowId);
        QVERIFY(!merge.ok);
    }

    void closingLastTabOfSecondaryWindowClosesIt() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        model.openTab({QStringLiteral("doc-b"), QStringLiteral("B")});
        const auto detach = model.detachTab(model.primaryWindowId(), 0);
        const WindowId detachedId = *detach.createdWindowId;

        const auto close = model.closeTab(detachedId, 0);
        QVERIFY(close.ok);
        QCOMPARE(close.closedWindowId, std::optional<WindowId>(detachedId));
        QVERIFY(model.window(detachedId) == nullptr);
        QCOMPARE(model.windows().size(), std::size_t(1));
    }

    void closingLastTabOfPrimaryWindowKeepsWindowEmpty() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        const auto close = model.closeTab(model.primaryWindowId(), 0);
        QVERIFY(close.ok);
        QVERIFY(!close.closedWindowId.has_value());

        const auto* primary = model.window(model.primaryWindowId());
        QVERIFY(primary != nullptr);
        QVERIFY(primary->tabs.empty());
        QCOMPARE(primary->activeIndex, -1);
        QCOMPARE(model.windows().size(), std::size_t(1));
    }

    void reorderTabMovesPositionAndTracksActive() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        model.openTab({QStringLiteral("doc-b"), QStringLiteral("B")});
        model.openTab({QStringLiteral("doc-c"), QStringLiteral("C")});
        model.setActiveTab(model.primaryWindowId(), 0);

        const auto result = model.reorderTab(model.primaryWindowId(), 0, 2);
        QVERIFY(result.ok);
        const auto* primary = model.window(model.primaryWindowId());
        QCOMPARE(primary->tabs[2].documentId, QStringLiteral("doc-a"));
        QCOMPARE(primary->activeIndex, 2);  // 使用中的頁籤要跟著移動
    }

    void mergeInsertsAtRequestedIndex() {
        TabLayoutModel model;
        model.openTab({QStringLiteral("doc-a"), QStringLiteral("A")});
        model.openTab({QStringLiteral("doc-b"), QStringLiteral("B")});
        const auto detach = model.detachTab(model.primaryWindowId(), 1);  // 分離 doc-b
        model.openTab({QStringLiteral("doc-c"), QStringLiteral("C")});    // 主視窗: a, c

        const auto merge = model.mergeWindow(*detach.createdWindowId, model.primaryWindowId(), 1);
        QVERIFY(merge.ok);
        const auto* primary = model.window(model.primaryWindowId());
        QCOMPARE(primary->tabs.size(), std::size_t(3));
        QCOMPARE(primary->tabs[0].documentId, QStringLiteral("doc-a"));
        QCOMPARE(primary->tabs[1].documentId, QStringLiteral("doc-b"));
        QCOMPARE(primary->tabs[2].documentId, QStringLiteral("doc-c"));
    }
};

QTEST_GUILESS_MAIN(TestTabLayoutModel)
#include "test_tab_layout_model.moc"
