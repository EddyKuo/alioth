#pragma once

// PRD-UI-002 指定的八個分頁（File / Home / View / Comment / Protect / Form /
// Organize / Help）的預設配置。
//
// 這裡只有 actionId 與標籤，沒有任何功能實作：Ribbon 的職責是「排出這些按鈕」，
// 至於按下去要做什麼，由整合端把對應 id 的 QAction 注入 ActionRegistry。
// 因此本檔可以在對應功能都還沒開工的情況下先存在，也不會反向依賴任何工作包。
//
// 使用者自訂的配置（PRD-UI-011）會覆寫這份預設；重設功能就是重新套用它。

#include "ui/ribbon/ribbon_model.h"

namespace alioth::ui::ribbon {

[[nodiscard]] Layout defaultLayout();

}  // namespace alioth::ui::ribbon
