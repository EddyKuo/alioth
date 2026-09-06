#pragma once

// 圖示名稱 → QIcon 的解析點。
//
// 設定檔裡的 icon 欄位只是一個字串，怎麼變成圖示是產品的決定（主題名稱、資源路徑、
// 或未來的圖示包）。Ribbon 把它留成可注入的函式，才不會為了畫一顆按鈕而綁死資源方案。

#include <QIcon>
#include <QString>

#include <functional>

namespace alioth::ui::ribbon {

using IconResolver = std::function<QIcon(const QString& iconName)>;

// 預設解析：先問系統圖示主題，再退回當成路徑或 Qt 資源。兩者都不中就回空圖示，
// 按鈕會只顯示文字——那比顯示一個問號佔位好，因為缺圖示不是錯誤。
[[nodiscard]] QIcon resolveIconByName(const QString& iconName);

}  // namespace alioth::ui::ribbon
