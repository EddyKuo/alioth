#pragma once

// 快捷鍵體系（PRD-UI-004）。
//
// 純邏輯核心：鍵位表可序列化、衝突偵測、匯入匯出、還原預設。偏好設定裡的快捷鍵
// 編輯頁只是這個模型的檢視，不持有任何鍵位邏輯——這樣鍵位表可以在無 GUI 環境下
// 完整測試，不需要 QApplication（見 tests/uisystem，QTEST_GUILESS_MAIN）。
//
// 衝突偵測刻意設計成「回報而不是靜默覆蓋」：同一情境下兩個動作綁同一個鍵，
// 若靜默讓後者贏，使用者會在某一天發現某個功能「按了沒反應」而且查不到原因——
// 那正是 IL-4「失敗快失敗明」要防止的情況。

#include <QByteArray>
#include <QKeySequence>
#include <QString>

#include <optional>
#include <vector>

namespace alioth::app {

// 快捷鍵作用的情境。Global 在任何情境下都啟用；其餘情境彼此互斥（不會同時啟用），
// 因此同一個鍵在不同非 Global 情境下可以重複使用而不算衝突——例如檢視情境的
// 單鍵工具切換與表單填寫情境的欄位巡覽不會同時搶同一個按鍵。
enum class ShortcutContext {
    Global,       // 檔案、視窗、復原重做——任何情境都可能觸發
    Viewer,       // 檢視、導覽、縮放
    Annotation,   // 註解工具（含註解工具的單鍵切換）
    FormFill,     // 表單填寫
    PageOrganize  // 頁面管理（組織窗格）
};

QString toString(ShortcutContext context);
std::optional<ShortcutContext> shortcutContextFromString(const QString& text);

struct ShortcutBinding {
    QString actionId;    // 與 ui::ribbon::ActionRegistry 同一套動作 id 命名
    QKeySequence sequence;
    ShortcutContext context{ShortcutContext::Global};
    QString description;  // 供編輯頁顯示，不參與比對與序列化語意判斷

    [[nodiscard]] bool isEmpty() const { return sequence.isEmpty(); }
};

// 一次衝突偵測的結果：同一個鍵位在互相看得見的情境裡被兩個以上動作占用。
struct ShortcutConflict {
    QKeySequence sequence;
    ShortcutContext context{ShortcutContext::Global};
    std::vector<QString> actionIds;  // 全部占用者，長度必然 >= 2
};

enum class BindResult {
    Ok,
    Conflict,      // 與既有綁定衝突，未套用
    UnknownAction  // actionId 不存在於目前的表中
};

class ShortcutScheme {
public:
    ShortcutScheme();

    // 對齊 Acrobat / PDF-XChange 的預設鍵位表（PRD-UI-004）。固定不變，供測試釘住。
    // 檔案/編輯/檢視類鍵位（Ctrl+O、Ctrl+S…）為業界公開且穩定多年的慣例；
    // 註解工具的單鍵切換（H/U/N…）取自 Acrobat 與 PDF-XChange 兩者公開的鍵盤
    // 快速鍵文件，惟未逐一以實機比對——若之後要做到「與 Acrobat 逐鍵相同」，
    // 需要 UI/UX 或 PM 以實機核對後在此表更新（見任務報告的偏離事項）。
    static std::vector<ShortcutBinding> defaultBindings();

    void resetToDefaults();
    void resetAction(const QString& actionId);

    // 嘗試把 actionId 的鍵位改成 sequence。衝突時回傳 Conflict 並「不」套用，
    // 呼叫端（偏好設定 UI）憑此決定要不要提示使用者先解除既有綁定。
    BindResult setBinding(const QString& actionId, const QKeySequence& sequence);

    // 清空鍵位（允許使用者移除快捷鍵）；一定成功，因為空鍵不會與任何鍵衝突。
    void clearBinding(const QString& actionId);

    [[nodiscard]] std::optional<ShortcutBinding> binding(const QString& actionId) const;
    [[nodiscard]] const std::vector<ShortcutBinding>& bindings() const { return bindings_; }

    // 掃描整張表，回報所有衝突。setBinding 已經把單一變更擋在入口，正常操作下
    // 這裡應該永遠是空的；但匯入外部檔案繞過了 setBinding 的逐一檢查，
    // 匯入後一定要重跑一次（importFromJson 內部已經這麼做）。
    [[nodiscard]] std::vector<ShortcutConflict> findConflicts() const;

    // JSON 匯出入。匯入時若整批資料本身互相衝突，整批拒絕而不是「盡量套用」——
    // 否則使用者分不出哪些鍵位其實沒生效（IL-4）。格式錯誤與衝突分開回報。
    [[nodiscard]] QByteArray exportToJson() const;

    struct ImportResult {
        bool ok{false};
        std::vector<ShortcutConflict> conflicts;  // ok == false 且 error 為空時，原因在這裡
        QString error;                            // 格式錯誤等，非衝突
    };
    ImportResult importFromJson(const QByteArray& json);

private:
    std::vector<ShortcutBinding> bindings_;
};

}  // namespace alioth::app
