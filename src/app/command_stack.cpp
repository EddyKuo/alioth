#include "app/command_stack.h"

namespace alioth::app {

CommandStack::CommandStack(QObject* parent) : QObject(parent) {}

bool CommandStack::push(Command command) {
    if (!command.redo || !command.undo) return false;

    if (!command.redo()) {
        // 失敗的操作不進歷史。若讓它進去，使用者按復原會去撤銷一件沒發生過的事。
        return false;
    }

    // 推入新命令時，游標之後的重做歷史作廢——那是另一條時間線。
    commands_.resize(cursor_);
    commands_.push_back(std::move(command));
    cursor_ = commands_.size();

    if (commands_.size() > kMaxDepth) {
        const std::size_t drop = commands_.size() - kMaxDepth;
        commands_.erase(commands_.begin(), commands_.begin() + static_cast<std::ptrdiff_t>(drop));
        cursor_ -= drop;
        // 存檔點被擠出歷史後就再也回不去了，此時一律視為 dirty，
        // 而不是讓 savedCursor_ 指向一個已經不存在的位置。
        if (savedCursor_ >= drop) {
            savedCursor_ -= drop;
        } else {
            savedCursor_ = static_cast<std::size_t>(-1);
        }
    }

    emit changed();
    return true;
}

bool CommandStack::undo() {
    if (!canUndo()) return false;
    const std::size_t index = cursor_ - 1;
    if (!commands_[index].undo()) return false;
    cursor_ = index;
    emit changed();
    return true;
}

bool CommandStack::redo() {
    if (!canRedo()) return false;
    if (!commands_[cursor_].redo()) return false;
    ++cursor_;
    emit changed();
    return true;
}

void CommandStack::clear() {
    commands_.clear();
    cursor_ = 0;
    savedCursor_ = 0;
    emit changed();
}

QString CommandStack::undoLabel() const {
    return canUndo() ? commands_[cursor_ - 1].label : QString();
}

QString CommandStack::redoLabel() const {
    return canRedo() ? commands_[cursor_].label : QString();
}

}  // namespace alioth::app
