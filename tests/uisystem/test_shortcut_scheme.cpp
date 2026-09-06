// 快捷鍵體系測試（PRD-UI-004）。
//
// 重點：預設表本身沒有內部衝突（否則就是自打嘴巴）、衝突偵測不會漏、
// setBinding 在偵測到衝突時真的沒有套用（不是「回報衝突但還是改了」）、
// 匯入匯出往返、還原預設。

#include <QtTest>

#include "app/uisystem/shortcut_scheme.h"

using namespace alioth::app;

class TestShortcutScheme : public QObject {
    Q_OBJECT

private slots:
    void defaultBindingsHaveNoInternalConflict() {
        ShortcutScheme scheme;
        const auto conflicts = scheme.findConflicts();
        for (const auto& c : conflicts) {
            qWarning() << "衝突鍵位:" << c.sequence.toString() << c.actionIds;
        }
        QVERIFY2(conflicts.empty(), "預設鍵位表本身不應該有衝突");
    }

    void defaultBindingsCoverKnownActions() {
        ShortcutScheme scheme;
        const auto save = scheme.binding(QStringLiteral("file.save"));
        QVERIFY(save.has_value());
        QCOMPARE(save->sequence, QKeySequence(QStringLiteral("Ctrl+S")));

        const auto undo = scheme.binding(QStringLiteral("edit.undo"));
        QVERIFY(undo.has_value());
        QCOMPARE(undo->sequence, QKeySequence(QStringLiteral("Ctrl+Z")));
    }

    void setBindingDetectsConflictAndDoesNotApply() {
        ShortcutScheme scheme;
        // file.save 是 Ctrl+S；把 file.print 也改成 Ctrl+S 應該衝突。
        const auto result = scheme.setBinding(QStringLiteral("file.print"), QKeySequence(QStringLiteral("Ctrl+S")));
        QCOMPARE(result, BindResult::Conflict);

        // 確認真的沒套用——file.print 應該還是原本的 Ctrl+P。
        const auto print = scheme.binding(QStringLiteral("file.print"));
        QVERIFY(print.has_value());
        QCOMPARE(print->sequence, QKeySequence(QStringLiteral("Ctrl+P")));
    }

    void setBindingSucceedsWhenNoConflict() {
        ShortcutScheme scheme;
        const auto result = scheme.setBinding(QStringLiteral("file.print"), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(result, BindResult::Ok);
        QCOMPARE(scheme.binding(QStringLiteral("file.print"))->sequence,
                 QKeySequence(QStringLiteral("Ctrl+Alt+P")));
    }

    void setBindingUnknownActionReported() {
        ShortcutScheme scheme;
        const auto result = scheme.setBinding(QStringLiteral("no.such.action"), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(result, BindResult::UnknownAction);
    }

    void sameKeyDifferentContextsDoNotConflict() {
        // tool.hand（Viewer, H）與 tool.sticky_note 不同鍵；驗證「非 Global 不同情境
        // 同鍵不算衝突」這條規則本身：手動建一個場景。
        ShortcutScheme scheme;
        // 把一個 PageOrganize 動作與一個 Annotation 動作設成同一個鍵，應該不衝突。
        QCOMPARE(scheme.setBinding(QStringLiteral("page.insert"), QKeySequence(Qt::Key_U)), BindResult::Ok);
        // tool.highlight 預設就是 U（Annotation 情境）；不同情境，不應該擋。
        const auto conflicts = scheme.findConflicts();
        QVERIFY(conflicts.empty());
    }

    void globalConflictsWithAnyContext() {
        ShortcutScheme scheme;
        // file.open 是 Global 的 Ctrl+O。把一個 Viewer 動作也設成 Ctrl+O 應該衝突，
        // 因為 Global 對任何情境都可見。
        const auto result = scheme.setBinding(QStringLiteral("view.zoom_in"), QKeySequence(QStringLiteral("Ctrl+O")));
        QCOMPARE(result, BindResult::Conflict);
    }

    void resetActionRestoresDefault() {
        ShortcutScheme scheme;
        QCOMPARE(scheme.setBinding(QStringLiteral("file.print"), QKeySequence(QStringLiteral("Ctrl+Alt+P"))), BindResult::Ok);
        scheme.resetAction(QStringLiteral("file.print"));
        QCOMPARE(scheme.binding(QStringLiteral("file.print"))->sequence, QKeySequence(QStringLiteral("Ctrl+P")));
    }

    void resetToDefaultsRestoresEverything() {
        ShortcutScheme scheme;
        scheme.clearBinding(QStringLiteral("file.save"));
        QVERIFY(scheme.binding(QStringLiteral("file.save"))->isEmpty());
        scheme.resetToDefaults();
        QCOMPARE(scheme.binding(QStringLiteral("file.save"))->sequence, QKeySequence(QStringLiteral("Ctrl+S")));
    }

    void exportImportRoundTrip() {
        ShortcutScheme scheme;
        QCOMPARE(scheme.setBinding(QStringLiteral("file.print"), QKeySequence(QStringLiteral("Ctrl+Alt+P"))), BindResult::Ok);
        const QByteArray json = scheme.exportToJson();

        ShortcutScheme restored;
        restored.resetToDefaults();
        const auto result = restored.importFromJson(json);
        QVERIFY(result.ok);
        QCOMPARE(restored.binding(QStringLiteral("file.print"))->sequence,
                 QKeySequence(QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(restored.bindings().size(), scheme.bindings().size());
    }

    void importRejectsConflictingBatchWithoutPartialApply() {
        ShortcutScheme scheme;
        const auto before = scheme.exportToJson();

        // 手工構造一份會衝突的匯入資料：把 file.save 也綁到 Ctrl+O。
        QJsonDocument doc = QJsonDocument::fromJson(before);
        QJsonObject root = doc.object();
        QJsonArray bindings = root.value(QStringLiteral("bindings")).toArray();
        for (int i = 0; i < bindings.size(); ++i) {
            QJsonObject obj = bindings[i].toObject();
            if (obj.value(QStringLiteral("actionId")).toString() == QStringLiteral("file.save")) {
                obj[QStringLiteral("sequence")] = QKeySequence(QStringLiteral("Ctrl+O")).toString(QKeySequence::PortableText);
                bindings[i] = obj;
            }
        }
        root[QStringLiteral("bindings")] = bindings;
        doc.setObject(root);

        const auto result = scheme.importFromJson(doc.toJson());
        QVERIFY(!result.ok);
        QVERIFY(!result.conflicts.empty());

        // 整批拒絕：file.save 應該仍是匯入前的值（Ctrl+S），不是「盡量套用」。
        QCOMPARE(scheme.binding(QStringLiteral("file.save"))->sequence, QKeySequence(QStringLiteral("Ctrl+S")));
    }

    void importRejectsMalformedJson() {
        ShortcutScheme scheme;
        const auto result = scheme.importFromJson(QByteArrayLiteral("{not valid json"));
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
    }

    void clearBindingNeverConflicts() {
        ShortcutScheme scheme;
        scheme.clearBinding(QStringLiteral("file.save"));
        scheme.clearBinding(QStringLiteral("file.print"));
        QVERIFY(scheme.findConflicts().empty());
    }
};

QTEST_GUILESS_MAIN(TestShortcutScheme)
#include "test_shortcut_scheme.moc"
