#pragma once

// 分頁標題控制與文件重新命名（PRD-UI-017）。
//
// 兩件不同的事，容易被混為一談：
//   1. 「分頁標題」是顯示層的標籤，可以被使用者暫時改寫（session 內有效），
//      與磁碟上的檔名無關——多開同一份檔案的兩個頁籤時特別有用。
//   2. 「文件重新命名」是真的把磁碟上的檔案改名，屬於檔案系統操作，本模組
//      只負責算出「新路徑該是什麼、有沒有衝突」這個純邏輯，真正呼叫
//      QFile::rename（或 platform 層的等價物）留給呼叫端——這裡不做檔案 I/O，
//      以維持可測試性（純函數，不接觸磁碟）。

#include <QString>

#include <vector>

namespace alioth::app {

struct TabTitleState {
    QString filePath;     // 磁碟路徑，可能為空（尚未存檔的新文件）
    QString customLabel;  // 使用者自訂的頁籤標籤；空字串代表「未自訂，用檔名」
    bool dirty{false};    // 有未儲存的變更
};

class TabTitleModel {
public:
    // 頁籤顯示文字：自訂標籤優先；否則取檔名（不含路徑、不含副檔名）；
    // 都沒有則是「未命名文件」。dirty 時附加星號，比照多數編輯器慣例。
    [[nodiscard]] static QString displayTitle(const TabTitleState& state);

    // 完整路徑做為 tooltip：自訂標籤模式下使用者容易忘記真正開的是哪個檔案，
    // tooltip 必須永遠給真相。
    [[nodiscard]] static QString tooltipText(const TabTitleState& state);

    // 多個頁籤顯示同一個檔名（不同路徑）時的消歧規則：在標題後附加父目錄名稱。
    // 傳入目前所有頁籤的路徑，回傳每個頁籤「是否需要消歧、消歧後文字」。
    struct DisambiguatedTitle {
        QString title;
        bool disambiguated{false};
    };
    [[nodiscard]] static std::vector<DisambiguatedTitle> disambiguate(const std::vector<TabTitleState>& states);

    // 重新命名磁碟檔案的目標路徑計算（純邏輯，不做 I/O）。newBaseName 不含副檔名時
    // 自動沿用原副檔名；newBaseName 含路徑分隔字元視為不合法。
    struct RenamePlan {
        bool ok{false};
        QString error;
        QString newPath;  // ok == true 時有效
    };
    [[nodiscard]] static RenamePlan planRename(const QString& currentPath, const QString& newBaseName);
};

}  // namespace alioth::app
