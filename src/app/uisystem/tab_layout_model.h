#pragma once

// 多頁籤與頁籤分離／合併的狀態轉移模型（PRD-UI-001）。
//
// 這是純邏輯模型：「哪份文件在哪個視窗的哪個位置」、「拖出頁籤變成獨立視窗」、
// 「拖回去合併」、「關掉最後一個頁籤時的行為」全部在這裡決定，不牽涉任何 Qt
// widget。真正的 QTabBar / QMainWindow 只是把使用者的拖曳手勢翻譯成這裡的
// 操作呼叫，並把回傳結果套用到畫面——widget 是薄殼，狀態轉移在這裡測。
//
// 視窗身分：第一個建立的視窗是「主視窗」（primaryWindowId()），關掉它最後一個
// 頁籤時不會消滅視窗本身（使用者仍需要一個地方開新檔案），而是進入「空」狀態；
// 其餘（分離出去的）視窗關掉最後一個頁籤時，視窗本身也隨之關閉——這與
// PDF-XChange / Acrobat 的行為一致：主視窗是常駐的工作區，子視窗是暫時的。

#include <QString>

#include <optional>
#include <vector>

namespace alioth::app {

using WindowId = int;
using DocumentId = QString;

struct TabState {
    DocumentId documentId;
    QString title;  // 顯示標題，來自 TabTitleModel，此處只是攜帶值
};

struct WindowState {
    WindowId id{-1};
    std::vector<TabState> tabs;
    int activeIndex{-1};  // tabs 為空時為 -1
};

// 一次操作後，呼叫端需要知道的畫面層級後果。
struct TabOperationResult {
    bool ok{false};
    QString error;
    // 若這次操作導致某個視窗被關閉（合併後的來源視窗、或分離視窗關掉最後一頁），
    // 記錄是哪一個，讓呼叫端可以真的關掉那個 QMainWindow。
    std::optional<WindowId> closedWindowId;
    // 若這次操作新建了一個視窗（detachTab），記錄新視窗 id。
    std::optional<WindowId> createdWindowId;
};

class TabLayoutModel {
public:
    TabLayoutModel();

    [[nodiscard]] WindowId primaryWindowId() const { return primaryWindowId_; }
    [[nodiscard]] const std::vector<WindowState>& windows() const { return windows_; }
    [[nodiscard]] const WindowState* window(WindowId id) const;
    // 回傳某份文件目前所在的 (windowId, tabIndex)；找不到回傳 nullopt。
    [[nodiscard]] std::optional<std::pair<WindowId, int>> locate(const DocumentId& documentId) const;

    // 在指定視窗開啟一份文件（新增頁籤並設為使用中）。windowId 省略時開在主視窗。
    TabOperationResult openTab(const TabState& tab, std::optional<WindowId> windowId = std::nullopt);

    // 關閉指定視窗裡的某個頁籤。若該視窗因此變空：
    //   - 主視窗 → 保留視窗，activeIndex 變 -1（空狀態），closedWindowId 不設定
    //   - 其餘視窗 → 視窗本身關閉，closedWindowId 設定為該視窗 id
    TabOperationResult closeTab(WindowId windowId, int tabIndex);

    // 把某個頁籤從原視窗中拖出，成立一個新的獨立視窗。若原視窗因此變空，
    // 比照 closeTab 的規則處理（主視窗保留、其餘視窗關閉）。
    TabOperationResult detachTab(WindowId sourceWindowId, int tabIndex);

    // 把 sourceWindowId 整個視窗（通常是先前 detach 出去的）合併回
    // targetWindowId，頁籤插入到 insertIndex（-1 代表插到最後）。
    // sourceWindowId 本身會被關閉。不可把主視窗當來源合併掉。
    TabOperationResult mergeWindow(WindowId sourceWindowId, WindowId targetWindowId, int insertIndex = -1);

    // 在視窗內移動頁籤順序（同視窗拖曳排序，不牽涉分離／合併）。
    TabOperationResult reorderTab(WindowId windowId, int fromIndex, int toIndex);

    void setActiveTab(WindowId windowId, int tabIndex);

private:
    WindowState* mutableWindow(WindowId id);
    WindowId allocateWindowId();

    std::vector<WindowState> windows_;
    WindowId primaryWindowId_;
    WindowId nextWindowId_{0};
};

}  // namespace alioth::app
