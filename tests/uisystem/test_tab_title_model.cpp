// 分頁標題控制與文件重新命名測試（PRD-UI-017）。

#include <QtTest>

#include <QDir>

#include "app/uisystem/tab_title_model.h"

using namespace alioth::app;

class TestTabTitleModel : public QObject {
    Q_OBJECT

private slots:
    void displayTitleFallsBackToFileName() {
        TabTitleState state;
        state.filePath = QStringLiteral("C:/docs/report.pdf");
        QCOMPARE(TabTitleModel::displayTitle(state), QStringLiteral("report"));
    }

    void displayTitlePrefersCustomLabel() {
        TabTitleState state;
        state.filePath = QStringLiteral("C:/docs/report.pdf");
        state.customLabel = QStringLiteral("季度報告");
        QCOMPARE(TabTitleModel::displayTitle(state), QStringLiteral("季度報告"));
    }

    void displayTitleUnnamedWhenNoPath() {
        TabTitleState state;
        QCOMPARE(TabTitleModel::displayTitle(state), QStringLiteral("未命名文件"));
    }

    void dirtyStateAppendsMarker() {
        TabTitleState state;
        state.filePath = QStringLiteral("C:/docs/report.pdf");
        state.dirty = true;
        QCOMPARE(TabTitleModel::displayTitle(state), QStringLiteral("report *"));
    }

    void tooltipShowsFullPathRegardlessOfCustomLabel() {
        TabTitleState state;
        state.filePath = QStringLiteral("C:/docs/report.pdf");
        state.customLabel = QStringLiteral("季度報告");
        QCOMPARE(TabTitleModel::tooltipText(state), QDir::toNativeSeparators(state.filePath));
    }

    void disambiguateAppendsParentDirOnCollision() {
        TabTitleState a;
        a.filePath = QStringLiteral("C:/projects/alpha/report.pdf");
        TabTitleState b;
        b.filePath = QStringLiteral("C:/projects/beta/report.pdf");
        TabTitleState c;
        c.filePath = QStringLiteral("C:/projects/gamma/unique.pdf");

        const auto results = TabTitleModel::disambiguate({a, b, c});
        QCOMPARE(results.size(), std::size_t(3));
        QVERIFY(results[0].disambiguated);
        QVERIFY(results[1].disambiguated);
        QVERIFY(!results[2].disambiguated);
        QVERIFY(results[0].title.contains(QStringLiteral("alpha")));
        QVERIFY(results[1].title.contains(QStringLiteral("beta")));
        QCOMPARE(results[2].title, QStringLiteral("unique"));
    }

    void disambiguateSkipsCustomLabelledTabs() {
        TabTitleState a;
        a.filePath = QStringLiteral("C:/projects/alpha/report.pdf");
        a.customLabel = QStringLiteral("我的標籤");
        TabTitleState b;
        b.filePath = QStringLiteral("C:/projects/beta/report.pdf");

        const auto results = TabTitleModel::disambiguate({a, b});
        QVERIFY(!results[0].disambiguated);
        QCOMPARE(results[0].title, QStringLiteral("我的標籤"));
        QVERIFY(!results[1].disambiguated);  // 只有一個未自訂又叫 report，不撞名
    }

    void planRenameRejectsUnsavedDocument() {
        const auto plan = TabTitleModel::planRename(QString(), QStringLiteral("new_name"));
        QVERIFY(!plan.ok);
    }

    void planRenameRejectsPathSeparators() {
        const auto plan = TabTitleModel::planRename(QStringLiteral("C:/docs/a.pdf"), QStringLiteral("bad/name"));
        QVERIFY(!plan.ok);
    }

    void planRenameRejectsIllegalCharacters() {
        const auto plan = TabTitleModel::planRename(QStringLiteral("C:/docs/a.pdf"), QStringLiteral("bad:name"));
        QVERIFY(!plan.ok);
    }

    void planRenameKeepsExtensionWhenOmitted() {
        const auto plan = TabTitleModel::planRename(QStringLiteral("C:/docs/a.pdf"), QStringLiteral("renamed"));
        QVERIFY(plan.ok);
        QCOMPARE(plan.newPath, QStringLiteral("C:/docs/renamed.pdf"));
    }

    void planRenameRespectsExplicitExtension() {
        const auto plan = TabTitleModel::planRename(QStringLiteral("C:/docs/a.pdf"), QStringLiteral("renamed.pdf"));
        QVERIFY(plan.ok);
        QCOMPARE(plan.newPath, QStringLiteral("C:/docs/renamed.pdf"));
    }

    void planRenameRejectsSameName() {
        const auto plan = TabTitleModel::planRename(QStringLiteral("C:/docs/a.pdf"), QStringLiteral("a.pdf"));
        QVERIFY(!plan.ok);
    }
};

QTEST_GUILESS_MAIN(TestTabTitleModel)
#include "test_tab_title_model.moc"
