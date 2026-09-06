// 工具持續模式測試（PRD-UI-007）。

#include <QtTest>

#include "app/uisystem/tool_persistence.h"

using namespace alioth::app;

class TestToolPersistence : public QObject {
    Q_OBJECT

private slots:
    void defaultsToNotSticky() {
        ToolPersistenceModel model;
        QVERIFY(!model.stickyEnabled());
    }

    void nonStickyModeFallsBackToSelectTool() {
        ToolPersistenceModel model(false);
        QCOMPARE(model.nextTool(QStringLiteral("tool.highlight")), QStringLiteral("tool.select"));
    }

    void stickyModeStaysOnSameTool() {
        ToolPersistenceModel model(true);
        QCOMPARE(model.nextTool(QStringLiteral("tool.highlight")), QStringLiteral("tool.highlight"));
    }

    void customSelectToolIdIsRespected() {
        ToolPersistenceModel model(false);
        QCOMPARE(model.nextTool(QStringLiteral("tool.highlight"), QStringLiteral("tool.arrow")),
                 QStringLiteral("tool.arrow"));
    }

    void toggleAtRuntimeChangesBehaviourImmediately() {
        ToolPersistenceModel model(false);
        QCOMPARE(model.nextTool(QStringLiteral("tool.highlight")), QStringLiteral("tool.select"));
        model.setStickyEnabled(true);
        QCOMPARE(model.nextTool(QStringLiteral("tool.highlight")), QStringLiteral("tool.highlight"));
    }
};

QTEST_GUILESS_MAIN(TestToolPersistence)
#include "test_tool_persistence.moc"
