#pragma once

// 命令匯流排與復原堆疊（WBS 3.13，PRD-ANN-010 要求 ≥ 100 步）。
//
// CLAUDE.md 的規則是「所有文件修改包成命令物件」。那條規則不是為了整齊，
// 是為了兩件具體的事：復原重做要涵蓋每一個修改（漏掉一個，使用者按 Ctrl+Z
// 就會得到不一致的文件），以及「有沒有未存檔內容」必須有單一真相來源。
//
// 這裡刻意不用 QUndoStack：它把命令綁在 QUndoCommand 的繼承階層上，
// 而我們的命令需要跨執行緒執行（存檔在引擎執行緒上），用 std::function
// 加上明確的執行結果比較好推理。

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace alioth::app {

// 一則可復原的修改。
//
// redo 與 undo 都必須是冪等且對稱的：對同一份文件狀態執行 redo 再 undo，
// 必須回到出發點。做不到對稱的操作（例如攤平註解）不該進這個堆疊，
// 而該明確標成不可復原並在 UI 上警告。
struct Command {
    QString label;                  // 顯示在「復原 XXX」選單上
    std::function<bool()> redo;     // 回傳是否成功；失敗不得改變狀態
    std::function<bool()> undo;
};

class CommandStack : public QObject {
    Q_OBJECT

public:
    // 上限存在的理由是記憶體而不是規格：每一步都可能持有一份文件快照。
    // PRD-ANN-010 要求至少 100 步，這裡給 200。
    static constexpr std::size_t kMaxDepth = 200;

    explicit CommandStack(QObject* parent = nullptr);

    // 執行並推入堆疊。redo 失敗時不推入——失敗的操作不該出現在復原歷史裡。
    bool push(Command command);

    bool undo();
    bool redo();

    void clear();

    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < commands_.size(); }
    [[nodiscard]] QString undoLabel() const;
    [[nodiscard]] QString redoLabel() const;
    [[nodiscard]] std::size_t depth() const noexcept { return commands_.size(); }

    // 未存檔狀態：存檔時呼叫 markSaved()，之後只要游標離開該位置就是 dirty。
    // 這樣「復原到存檔當時的狀態」會正確地變回乾淨，而不是永遠顯示未存檔。
    void markSaved() noexcept { savedCursor_ = cursor_; }
    [[nodiscard]] bool isDirty() const noexcept { return cursor_ != savedCursor_; }

signals:
    void changed();

private:
    std::vector<Command> commands_;
    std::size_t cursor_{0};       // 下一個要 redo 的位置；cursor_ 之前的都已執行
    std::size_t savedCursor_{0};
};

}  // namespace alioth::app
