#pragma once

// 設定檔匯出／匯入／重設（PRD-UI-014）與企業部署用的自訂設定檔（PRD-UI-015）。
//
// 一份設定檔包含四樣東西，它們原本散在不同地方：偏好設定（QSettings）、
// 快捷鍵表、主題模式、Ribbon 配置。使用者要的是「把我這台機器的設定搬到
// 另一台」，而不是分四次匯出四個檔案。
//
// **不包含的東西**（刻意）：
//   - 最近檔案與文件歷史——那是個人足跡，跟著設定檔散出去等於洩漏
//     使用者看過哪些文件，而企業部署的範本更不該帶上製作者的紀錄
//   - 簽名庫——手寫簽名是個人資產，見 handwritten_signature.h
//
// 企業部署（PRD-UI-015）的用法是把匯出的 JSON 放到唯讀位置，由系統管理員
// 決定哪些項目鎖定。鎖定清單存在設定檔裡而不是程式裡，因為每個組織要鎖的
// 東西不同。

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

#include "app/uisystem/shortcut_scheme.h"
#include "app/uisystem/theme_controller.h"

namespace alioth::app {

class Settings;

struct SettingsProfile {
    // 版本號。格式變動時舊檔案要能被辨識出來並明確拒絕，而不是被解讀成
    // 一堆預設值——後者會讓使用者以為匯入成功了。
    int version{1};

    QString name;         // 給人看的名稱，例如「工程部標準設定」
    QString description;

    std::size_t cacheBytes{0};
    int autosaveSeconds{-1};   // -1 代表這份設定檔不管這一項
    QString authorName;
    std::optional<bool> nightMode;
    std::optional<ThemeMode> themeMode;

    QByteArray shortcutsJson;  // ShortcutScheme::exportToJson() 的原文
    QByteArray ribbonLayoutJson;

    // 企業部署：鎖定的項目使用者不可修改。以鍵名列舉（"cache"、"autosave"、
    // "theme"、"shortcuts"、"ribbon"），未列出的項目一律可改。
    QStringList lockedKeys;

    [[nodiscard]] bool isLocked(const QString& key) const { return lockedKeys.contains(key); }
};

struct ProfileImportResult {
    bool ok{false};
    QString diagnostic;
    QStringList appliedKeys;  // 實際套用了哪些項目
    QStringList skippedKeys;  // 因鎖定或資料無效而略過的項目
};

// 由目前的執行狀態產生一份設定檔。
[[nodiscard]] SettingsProfile captureProfile(const Settings& settings,
                                             const ShortcutScheme& shortcuts, ThemeMode theme,
                                             const QString& name = {});

[[nodiscard]] QByteArray exportProfile(const SettingsProfile& profile);

// 解析。版本不符或 JSON 壞掉時明確失敗，不做「盡量讀」——半套的設定
// 比完全沒匯入更難察覺。
[[nodiscard]] bool parseProfile(const QByteArray& json, SettingsProfile& out,
                                QString* diagnostic = nullptr);

// 套用到執行中的物件上。鎖定的項目會出現在 skippedKeys，呼叫端據此在 UI 上
// 把對應的控制項停用——套用之後才發現改不了，使用者會以為是壞的。
[[nodiscard]] ProfileImportResult applyProfile(const SettingsProfile& profile, Settings& settings,
                                               ShortcutScheme& shortcuts, ThemeMode* themeOut);

// 全部回復出廠值。回傳被重設的項目清單，讓 UI 能明確告知使用者發生了什麼——
// 「重設」按下去畫面沒變化時，使用者無法分辨是成功了還是壞了。
QStringList resetToFactoryDefaults(Settings& settings, ShortcutScheme& shortcuts,
                                   ThemeMode* themeOut);

}  // namespace alioth::app
