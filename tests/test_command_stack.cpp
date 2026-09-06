// 命令匯流排與復原堆疊測試。
//
// 復原語意寫錯不會崩潰，只會讓文件慢慢偏離使用者以為的狀態——
// 而使用者通常要到存檔之後才發現。所以邊界條件要逐條驗。

#include <QtTest>

#include <vector>

#include "app/command_stack.h"

using namespace alioth::app;

namespace {

// 用一個整數當作「文件狀態」，命令對它加減。
struct Counter {
    int value{0};
};

Command addCommand(Counter& counter, int delta, const QString& label = QStringLiteral("加")) {
    Command command;
    command.label = label;
    command.redo = [&counter, delta] {
        counter.value += delta;
        return true;
    };
    command.undo = [&counter, delta] {
        counter.value -= delta;
        return true;
    };
    return command;
}

}  // namespace

class TestCommandStack : public QObject {
    Q_OBJECT

private slots:
    void pushExecutesAndRecords() {
        Counter counter;
        CommandStack stack;
        QSignalSpy changed(&stack, &CommandStack::changed);

        QVERIFY(stack.push(addCommand(counter, 5)));
        QCOMPARE(counter.value, 5);
        QCOMPARE(stack.depth(), std::size_t{1});
        QVERIFY(stack.canUndo());
        QVERIFY(!stack.canRedo());
        QCOMPARE(changed.count(), 1);
    }

    void undoRedoIsSymmetric() {
        Counter counter;
        CommandStack stack;
        QVERIFY(stack.push(addCommand(counter, 3)));
        QVERIFY(stack.push(addCommand(counter, 7)));
        QCOMPARE(counter.value, 10);

        QVERIFY(stack.undo());
        QCOMPARE(counter.value, 3);
        QVERIFY(stack.undo());
        QCOMPARE(counter.value, 0);
        QVERIFY(!stack.canUndo());
        QVERIFY(!stack.undo());

        QVERIFY(stack.redo());
        QCOMPARE(counter.value, 3);
        QVERIFY(stack.redo());
        QCOMPARE(counter.value, 10);
        QVERIFY(!stack.redo());
    }

    void failedCommandDoesNotEnterHistory() {
        // 失敗的操作若進了歷史，使用者按復原會去撤銷一件沒發生過的事。
        Counter counter;
        CommandStack stack;

        Command failing;
        failing.label = QStringLiteral("會失敗的操作");
        failing.redo = [] { return false; };
        failing.undo = [&counter] {
            counter.value = -999;  // 不該被呼叫到
            return true;
        };

        QVERIFY(!stack.push(std::move(failing)));
        QCOMPARE(stack.depth(), std::size_t{0});
        QVERIFY(!stack.canUndo());
        QCOMPARE(counter.value, 0);
    }

    void commandWithoutHandlersIsRejected() {
        CommandStack stack;
        Command empty;
        empty.label = QStringLiteral("空命令");
        QVERIFY(!stack.push(std::move(empty)));
        QCOMPARE(stack.depth(), std::size_t{0});
    }

    void pushAfterUndoDiscardsRedoBranch() {
        Counter counter;
        CommandStack stack;
        QVERIFY(stack.push(addCommand(counter, 1)));
        QVERIFY(stack.push(addCommand(counter, 2)));
        QVERIFY(stack.undo());
        QCOMPARE(counter.value, 1);
        QVERIFY(stack.canRedo());

        // 在復原之後做新動作，原本的重做分支就是另一條時間線，必須丟掉。
        QVERIFY(stack.push(addCommand(counter, 10)));
        QVERIFY(!stack.canRedo());
        QCOMPARE(stack.depth(), std::size_t{2});
        QCOMPARE(counter.value, 11);
    }

    void dirtyTracksSavePoint() {
        Counter counter;
        CommandStack stack;
        QVERIFY(!stack.isDirty());

        QVERIFY(stack.push(addCommand(counter, 1)));
        QVERIFY(stack.isDirty());

        stack.markSaved();
        QVERIFY(!stack.isDirty());

        QVERIFY(stack.push(addCommand(counter, 1)));
        QVERIFY(stack.isDirty());

        // 復原回存檔當時的位置，應該重新變成乾淨的，而不是永遠顯示未存檔。
        QVERIFY(stack.undo());
        QVERIFY(!stack.isDirty());
    }

    void depthIsCappedAndKeepsMostRecent() {
        Counter counter;
        CommandStack stack;
        for (std::size_t i = 0; i < CommandStack::kMaxDepth + 50; ++i) {
            QVERIFY(stack.push(addCommand(counter, 1)));
        }
        QCOMPARE(stack.depth(), CommandStack::kMaxDepth);
        QCOMPARE(counter.value, static_cast<int>(CommandStack::kMaxDepth + 50));

        // 上限是記憶體考量，但 PRD-ANN-010 要求至少 100 步可復原。
        QVERIFY(CommandStack::kMaxDepth >= 100);
        for (std::size_t i = 0; i < 100; ++i) {
            QVERIFY(stack.undo());
        }
    }

    void savePointPushedOutOfHistoryMeansAlwaysDirty() {
        // 存檔點被擠出歷史後就再也回不去，此時必須一律視為未存檔，
        // 而不是讓它指向一個已經不存在的位置而誤報「乾淨」。
        Counter counter;
        CommandStack stack;
        QVERIFY(stack.push(addCommand(counter, 1)));
        stack.markSaved();
        QVERIFY(!stack.isDirty());

        for (std::size_t i = 0; i < CommandStack::kMaxDepth + 10; ++i) {
            QVERIFY(stack.push(addCommand(counter, 1)));
        }
        QVERIFY(stack.isDirty());
    }

    void clearResetsEverything() {
        Counter counter;
        CommandStack stack;
        QVERIFY(stack.push(addCommand(counter, 1)));
        stack.clear();
        QCOMPARE(stack.depth(), std::size_t{0});
        QVERIFY(!stack.canUndo());
        QVERIFY(!stack.canRedo());
        QVERIFY(!stack.isDirty());
    }

    void labelsFollowCursor() {
        Counter counter;
        CommandStack stack;
        QVERIFY(stack.push(addCommand(counter, 1, QStringLiteral("第一步"))));
        QVERIFY(stack.push(addCommand(counter, 1, QStringLiteral("第二步"))));
        QCOMPARE(stack.undoLabel(), QStringLiteral("第二步"));
        QVERIFY(stack.undo());
        QCOMPARE(stack.undoLabel(), QStringLiteral("第一步"));
        QCOMPARE(stack.redoLabel(), QStringLiteral("第二步"));
    }

    void failedUndoLeavesCursorUnchanged() {
        // 復原失敗時（例如檔案被別人改過）游標不能前進，否則歷史與實際狀態會脫節。
        Counter counter;
        CommandStack stack;

        Command command;
        command.label = QStringLiteral("無法復原");
        command.redo = [&counter] {
            counter.value += 1;
            return true;
        };
        command.undo = [] { return false; };

        QVERIFY(stack.push(std::move(command)));
        QVERIFY(!stack.undo());
        QVERIFY(stack.canUndo());
        QCOMPARE(counter.value, 1);
    }
};

QTEST_APPLESS_MAIN(TestCommandStack)
#include "test_command_stack.moc"
