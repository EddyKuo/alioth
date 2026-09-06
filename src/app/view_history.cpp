#include "app/view_history.h"

namespace alioth::app {

bool ViewHistory::record(const ViewPosition& position) {
    if (!entries_.empty() && entries_[cursor_] == position) {
        // 同一個位置不重複入堆。少了這一條，連按兩次同一個書籤之後，
        // 第一次按後退會停在原地——看起來就是「後退鍵壞了」。
        // 倍率仍然更新：位置一樣但使用者可能縮放過。
        entries_[cursor_].scale = position.scale;
        return false;
    }

    // 從歷史中間產生新導覽時，前方整段丟棄。與瀏覽器一致，
    // 也是唯一不會讓使用者迷路的語意——保留前方紀錄會讓「前進」跳到
    // 一條使用者已經離開的分支上。
    if (!entries_.empty()) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_) + 1, entries_.end());
    }

    entries_.push_back(position);

    // 超過上限時從最舊的丟。游標跟著往回移一格，否則它會指到別人身上。
    if (entries_.size() > kMaxEntries) {
        entries_.erase(entries_.begin());
    }
    cursor_ = entries_.size() - 1;
    return true;
}

ViewPosition ViewHistory::goBack() {
    if (!canGoBack()) return current();
    --cursor_;
    return entries_[cursor_];
}

ViewPosition ViewHistory::goForward() {
    if (!canGoForward()) return current();
    ++cursor_;
    return entries_[cursor_];
}

ViewPosition ViewHistory::current() const {
    if (entries_.empty()) return ViewPosition{};
    return entries_[cursor_];
}

void ViewHistory::clear() noexcept {
    entries_.clear();
    cursor_ = 0;
}

}  // namespace alioth::app
