#pragma once

// 第三方程式工具列的呈現層（PRD-UI-016，R3／C）。
//
// 這一層只做兩件事：讓使用者維護工具清單、在真正啟動前把完整命令列攤開來
// 給使用者確認。驗證規則與命令組裝全部在 app::external_tools（見
// app/external_tools.h），這裡不重複判斷合不合法，只負責顯示。

#include <QDialog>
#include <QString>

#include <vector>

#include "app/external_tools.h"

class QLineEdit;
class QListWidget;

namespace alioth::app {
class Settings;
}

namespace alioth::ui {

// 新增／編輯單一工具的對話框。exec() 之後用 result() 呼叫端可以直接讀
// tool() 取得使用者輸入的內容；是否合法由呼叫端另外用
// app::validateExternalTool 檢查——這裡故意不擋「確定」按鈕，讓使用者
// 先看到具體的錯誤說明（例如「找不到執行檔」）而不是按鈕一直是灰的、
// 卻不知道差在哪裡。
class ExternalToolEditDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExternalToolEditDialog(const app::ExternalTool& initial, QWidget* parent = nullptr);

    [[nodiscard]] app::ExternalTool tool() const;

private:
    void browseForExecutable();

    QLineEdit* name_{nullptr};
    QLineEdit* executablePath_{nullptr};
    QLineEdit* argumentsTemplate_{nullptr};
    QLineEdit* workingDirectory_{nullptr};
};

// 工具清單管理（新增／編輯／移除／儲存到 Settings）。
class ExternalToolManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExternalToolManagerDialog(app::Settings* settings, QWidget* parent = nullptr);

signals:
    // 清單變動並已寫回 Settings 後發出，呼叫端（MainWindow）藉此重建工具列。
    void toolsChanged();

private:
    void refreshList();
    void addTool();
    void editSelectedTool();
    void removeSelectedTool();
    void persist();

    app::Settings* settings_{nullptr};
    QListWidget* list_{nullptr};
    std::vector<app::ExternalTool> tools_;
};

// 啟動前的確認對話框（安全限制 #3：必須顯示完整命令列）。回傳使用者是否
// 按下「啟動」。這個函式不做任何驗證，呼叫端必須先驗證過工具設定合法、
// 再組出 request，這裡純粹是最後一道使用者確認。
[[nodiscard]] bool confirmExternalToolLaunch(const app::ExternalTool& tool,
                                             const app::ExternalToolLaunchRequest& request,
                                             QWidget* parent);

}  // namespace alioth::ui
