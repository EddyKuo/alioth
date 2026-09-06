#include "app/uisystem/tab_layout_model.h"

#include <algorithm>

namespace alioth::app {

TabLayoutModel::TabLayoutModel() {
    primaryWindowId_ = allocateWindowId();
    WindowState primary;
    primary.id = primaryWindowId_;
    windows_.push_back(primary);
}

WindowId TabLayoutModel::allocateWindowId() { return nextWindowId_++; }

const WindowState* TabLayoutModel::window(WindowId id) const {
    auto it = std::find_if(windows_.begin(), windows_.end(),
                            [&](const WindowState& w) { return w.id == id; });
    return it == windows_.end() ? nullptr : &(*it);
}

WindowState* TabLayoutModel::mutableWindow(WindowId id) {
    auto it = std::find_if(windows_.begin(), windows_.end(),
                            [&](const WindowState& w) { return w.id == id; });
    return it == windows_.end() ? nullptr : &(*it);
}

std::optional<std::pair<WindowId, int>> TabLayoutModel::locate(const DocumentId& documentId) const {
    for (const auto& w : windows_) {
        for (std::size_t i = 0; i < w.tabs.size(); ++i) {
            if (w.tabs[i].documentId == documentId) {
                return std::make_pair(w.id, static_cast<int>(i));
            }
        }
    }
    return std::nullopt;
}

TabOperationResult TabLayoutModel::openTab(const TabState& tab, std::optional<WindowId> windowId) {
    const WindowId target = windowId.value_or(primaryWindowId_);
    WindowState* w = mutableWindow(target);
    TabOperationResult result;
    if (!w) {
        result.error = QStringLiteral("視窗不存在");
        return result;
    }
    w->tabs.push_back(tab);
    w->activeIndex = static_cast<int>(w->tabs.size()) - 1;
    result.ok = true;
    return result;
}

namespace {
// 從視窗移除一個頁籤，回傳「視窗是否因此變空」；activeIndex 一併修正。
void removeTabAt(WindowState& w, int index) {
    w.tabs.erase(w.tabs.begin() + index);
    if (w.tabs.empty()) {
        w.activeIndex = -1;
    } else if (w.activeIndex >= static_cast<int>(w.tabs.size())) {
        w.activeIndex = static_cast<int>(w.tabs.size()) - 1;
    } else if (w.activeIndex > index) {
        --w.activeIndex;
    }
}
}  // namespace

TabOperationResult TabLayoutModel::closeTab(WindowId windowId, int tabIndex) {
    TabOperationResult result;
    WindowState* w = mutableWindow(windowId);
    if (!w || tabIndex < 0 || tabIndex >= static_cast<int>(w->tabs.size())) {
        result.error = QStringLiteral("頁籤索引無效");
        return result;
    }
    removeTabAt(*w, tabIndex);
    if (w->tabs.empty() && windowId != primaryWindowId_) {
        result.closedWindowId = windowId;
        windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
                                       [&](const WindowState& s) { return s.id == windowId; }),
                        windows_.end());
    }
    result.ok = true;
    return result;
}

TabOperationResult TabLayoutModel::detachTab(WindowId sourceWindowId, int tabIndex) {
    TabOperationResult result;
    WindowState* source = mutableWindow(sourceWindowId);
    if (!source || tabIndex < 0 || tabIndex >= static_cast<int>(source->tabs.size())) {
        result.error = QStringLiteral("頁籤索引無效");
        return result;
    }
    // 只有一個頁籤時分離沒有意義（分離出來的新視窗會跟原視窗長得一模一樣），
    // 呼叫端（拖曳手勢）應該在觸發前就擋掉這個情況；模型層仍拒絕以避免產生
    // 空的主視窗以外的視窗。
    if (source->tabs.size() == 1) {
        result.error = QStringLiteral("只有一個頁籤時無法分離");
        return result;
    }

    TabState moved = source->tabs[tabIndex];
    removeTabAt(*source, tabIndex);

    WindowState created;
    created.id = allocateWindowId();
    created.tabs.push_back(moved);
    created.activeIndex = 0;
    windows_.push_back(created);

    result.ok = true;
    result.createdWindowId = created.id;
    return result;
}

TabOperationResult TabLayoutModel::mergeWindow(WindowId sourceWindowId, WindowId targetWindowId,
                                                int insertIndex) {
    TabOperationResult result;
    if (sourceWindowId == primaryWindowId_) {
        result.error = QStringLiteral("主視窗不可被當作合併來源");
        return result;
    }
    if (sourceWindowId == targetWindowId) {
        result.error = QStringLiteral("來源與目標視窗相同");
        return result;
    }
    WindowState* source = mutableWindow(sourceWindowId);
    WindowState* target = mutableWindow(targetWindowId);
    if (!source || !target) {
        result.error = QStringLiteral("視窗不存在");
        return result;
    }

    const int at = (insertIndex < 0 || insertIndex > static_cast<int>(target->tabs.size()))
                       ? static_cast<int>(target->tabs.size())
                       : insertIndex;
    target->tabs.insert(target->tabs.begin() + at, source->tabs.begin(), source->tabs.end());
    target->activeIndex = at;

    windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
                                   [&](const WindowState& s) { return s.id == sourceWindowId; }),
                    windows_.end());

    result.ok = true;
    result.closedWindowId = sourceWindowId;
    return result;
}

TabOperationResult TabLayoutModel::reorderTab(WindowId windowId, int fromIndex, int toIndex) {
    TabOperationResult result;
    WindowState* w = mutableWindow(windowId);
    if (!w || fromIndex < 0 || fromIndex >= static_cast<int>(w->tabs.size()) || toIndex < 0 ||
        toIndex >= static_cast<int>(w->tabs.size())) {
        result.error = QStringLiteral("頁籤索引無效");
        return result;
    }
    TabState moved = w->tabs[fromIndex];
    w->tabs.erase(w->tabs.begin() + fromIndex);
    w->tabs.insert(w->tabs.begin() + toIndex, moved);
    if (w->activeIndex == fromIndex) {
        w->activeIndex = toIndex;
    }
    result.ok = true;
    return result;
}

void TabLayoutModel::setActiveTab(WindowId windowId, int tabIndex) {
    WindowState* w = mutableWindow(windowId);
    if (!w || tabIndex < 0 || tabIndex >= static_cast<int>(w->tabs.size())) return;
    w->activeIndex = tabIndex;
}

}  // namespace alioth::app
