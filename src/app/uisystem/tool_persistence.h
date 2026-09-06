#pragma once

// 工具持續模式（PRD-UI-007）。
//
// 預設行為（多數檢視器）：用完一個註解工具後自動切回選取工具，避免使用者
// 誤加下一個註解。「工具持續模式」開啟時則相反：用完工具後停留在原工具，
// 方便連續加同一種註解（例如連續畫多個螢光筆）。
//
// 這條規則本身沒有 UI 也沒有計時器，是一個純函數：「用完一個工具之後，
// 下一個作用中的工具該是什麼」。呼叫端（PageView 或工具列）在每次完成一次
// 註解操作後呼叫 nextTool()，把結果設成目前作用中的工具。

#include <QString>

namespace alioth::app {

class ToolPersistenceModel {
public:
    explicit ToolPersistenceModel(bool stickyEnabled = false) : stickyEnabled_(stickyEnabled) {}

    [[nodiscard]] bool stickyEnabled() const { return stickyEnabled_; }
    void setStickyEnabled(bool enabled) { stickyEnabled_ = enabled; }

    // usedToolId：剛完成操作的工具 id（例如 "tool.highlight"）。
    // 回傳完成操作後應該作用中的工具 id：持續模式下就是 usedToolId 本身；
    // 否則退回選取工具。selectToolId 讓呼叫端自訂「選取工具」的 id，
    // 不在此處寫死字面量。
    [[nodiscard]] QString nextTool(const QString& usedToolId,
                                    const QString& selectToolId = QStringLiteral("tool.select")) const {
        return stickyEnabled_ ? usedToolId : selectToolId;
    }

private:
    bool stickyEnabled_;
};

}  // namespace alioth::app
