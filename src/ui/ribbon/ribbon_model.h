#pragma once

// Ribbon 的資料模型（PRD-UI-002 / PRD-UI-010 / PRD-UI-011）。
//
// Ribbon 的內容是資料而不是程式碼，這是「自訂 Ribbon」這條需求的前提：
// 使用者要能自組分頁，就不可能把分頁寫死在建構函式裡。
//
// 這一層刻意不依賴 QWidget 也不認識任何具體功能，只知道字串形式的 actionId。
// 兩個好處：模型可以在沒有 GUI 的情況下單元測試；Ribbon 不會因為需要
// 「知道複製要做什麼」而長成第二個 main_window。

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace alioth::ui::ribbon {

// 大按鈕為圖示在上、文字在下的單顆；小按鈕為圖示在左、文字在右，並三顆一欄堆疊。
// 這兩種尺寸就是 Office 系 Ribbon 的全部排列規則，不需要更複雜的佈局描述。
enum class ItemSize { Large, Small };

enum class ItemType { Action, Separator };

struct Item {
    ItemType type{ItemType::Action};
    // 掛載點。Ribbon 只保存這個字串，真正的 QAction 由整合端注入 ActionRegistry。
    QString actionId;
    // 空字串表示沿用 QAction 的文字；填了就覆寫，讓使用者能替自訂分頁改名。
    QString label;
    // 圖示名稱或資源路徑，交給整合端解析。模型不碰 QIcon，避免把 QtGui 拉進資料層。
    QString iconName;
    ItemSize size{ItemSize::Large};
    // 空字串表示交給 assignAutomaticKeyTips() 推導。
    QString keyTip;

    friend bool operator==(const Item&, const Item&) = default;
};

struct Group {
    QString id;
    QString title;
    QString keyTip;
    QList<Item> items;

    friend bool operator==(const Group&, const Group&) = default;
};

struct Page {
    QString id;
    QString title;
    QString keyTip;
    QList<Group> groups;

    friend bool operator==(const Page&, const Page&) = default;
};

struct Layout {
    // 設定檔格式版本。與產品版本無關，只在模型結構有破壞性變更時才動。
    int version{1};
    QList<Page> pages;
    // 快速存取工具列。它獨立於分頁之外，所以只存 actionId 清單。
    QStringList quickAccessActionIds;
    bool collapsed{false};

    friend bool operator==(const Layout&, const Layout&) = default;

    [[nodiscard]] bool isEmpty() const noexcept { return pages.isEmpty(); }
};

// 解析結果。設定檔是使用者可以手改的檔案，畸形內容是常態而非例外，
// 因此解析採「盡量救回」策略：能用的節點保留，壞掉的節點跳過並記進 warnings，
// 只有連根節點都不是 JSON 物件時才算 error。呼叫端據此決定要不要退回預設配置。
struct ParseResult {
    QString error;
    QStringList warnings;

    [[nodiscard]] bool ok() const noexcept { return error.isEmpty(); }
};

[[nodiscard]] QJsonObject toJson(const Layout& layout);
[[nodiscard]] Layout fromJson(const QJsonObject& root, ParseResult* result = nullptr);

// 檔案層級的存取。失敗時回傳空 Layout 並在 result 中說明，不丟例外——
// 設定檔讀不到只該讓 Ribbon 退回預設配置，不該讓程式起不來。
[[nodiscard]] bool saveToFile(const Layout& layout, const QString& path, ParseResult* result = nullptr);
[[nodiscard]] Layout loadFromFile(const QString& path, ParseResult* result = nullptr);

// 補齊未指定的 KeyTip。優先取標籤的第一個 ASCII 英數字，衝突時往後找未用過的字元。
// 不在 fromJson 裡自動做，是為了讓序列化往返保持等值：讀進來什麼樣，存出去就什麼樣。
void assignAutomaticKeyTips(Layout& layout);

// 收集配置中出現的所有 actionId（含快速存取列），供整合端檢查有哪些 id 需要注入。
[[nodiscard]] QStringList collectActionIds(const Layout& layout);

}  // namespace alioth::ui::ribbon
