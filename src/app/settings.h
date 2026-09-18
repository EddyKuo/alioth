#pragma once

// 偏好設定（PRD-UI-008）與最近檔案（PRD-IO-006）。
//
// 這一層存在的理由是把「設定的預設值與合法範圍」放在一個地方。散在各個使用點時，
// 快取上限會出現三個不同的預設值，而且沒有人知道哪個才是對的。
//
// 實際儲存走 QSettings，路徑由 platform::configDirectory() 決定。

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <vector>

#include "app/external_tools.h"
#include "app/uisystem/cursor_scale.h"
#include "app/uisystem/locale_manager.h"

namespace alioth::app {

class Settings : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxRecentFiles = 12;

    explicit Settings(QObject* parent = nullptr);

    // 圖磚快取上限。範圍由 PRD §4.3 定義（128–1024 MB），寫入時夾住。
    [[nodiscard]] std::size_t cacheBytes() const;
    void setCacheBytes(std::size_t bytes);

    // 自動儲存間隔（秒）。0 代表關閉——關閉是使用者的權利，但預設是開的。
    [[nodiscard]] int autosaveSeconds() const;
    void setAutosaveSeconds(int seconds);

    // 註解作者名稱。空字串時由呼叫端退回系統使用者名稱。
    [[nodiscard]] QString authorName() const;
    void setAuthorName(const QString& name);

    [[nodiscard]] bool nightMode() const;
    void setNightMode(bool enabled);

    // 可調整游標大小（PRD-UI-018）。
    [[nodiscard]] CursorSizeLevel cursorSizeLevel() const;
    void setCursorSizeLevel(CursorSizeLevel level);

    // 工具持續模式（PRD-UI-007）。預設關閉：用完一個註解工具就切回選取，
    // 避免使用者在下一次點擊時誤加一個註解。
    [[nodiscard]] bool stickyTools() const;
    void setStickyTools(bool enabled);

    // 介面語言（PRD-UI-005）。沒設定過時跟隨系統語言。
    [[nodiscard]] Locale locale() const;
    void setLocale(Locale locale);

    // 觸控模式（PRD-UI-013）。開啟時 Ribbon 按鈕放大到 44 CSS px 等效。
    // 預設關閉：滑鼠使用者按 44px 的按鈕沒有好處，而整條 Ribbon 會明顯變高。
    [[nodiscard]] bool touchMode() const;
    void setTouchMode(bool enabled);

    // 自訂快捷鍵（PRD-UI-004）。存的是 ShortcutScheme::exportToJson() 的原文——
    // 逐鍵拆成 QSettings 條目會讓「設定檔比程式新」變成一堆孤兒鍵，而整段 JSON
    // 由 importFromJson 一次驗證，格式不合就整批不套用，落回預設鍵位。
    // 空字串代表沒有自訂過。
    [[nodiscard]] QByteArray shortcutsJson() const;
    void setShortcutsJson(const QByteArray& json);

    // 第三方程式工具列（PRD-UI-016）。清單本身可以含未通過驗證的項目
    // （例如使用者換了電腦、原本設定的執行檔路徑不存在了）——是否要在
    // UI 上標成失效，由呼叫端拿 validateExternalTool() 逐一檢查決定，
    // 讀取這裡不做過濾，才不會讓使用者設定過的工具無聲消失。
    [[nodiscard]] std::vector<ExternalTool> externalTools() const;
    void setExternalTools(const std::vector<ExternalTool>& tools);

    // 使用者自行加入的信任根憑證檔（PDF-XChange 的 Digital IDs）。
    //
    // 只存路徑不存憑證內容：信任根是公開資料不是秘密，但**它是一個信任決定**，
    // 而把決定的依據複製一份藏在設定檔裡，會讓「這台機器信任哪些憑證」變成
    // 兩個真相。路徑失效時由 UI 明確標示，不靜默移除——使用者換過機器之後
    // 需要知道自己原本信任的是哪幾張。
    //
    // 清單為空是合理的預設：PRD §4.1 的立場是信任判斷三平台一致，
    // 因此刻意不自動載入作業系統的憑證存放區。
    [[nodiscard]] QStringList trustedCertificateFiles() const;
    void setTrustedCertificateFiles(const QStringList& paths);

    // 最近檔案，最新的在最前面。重複開啟同一個檔案只會把它移到最前，不會出現兩次。
    [[nodiscard]] QStringList recentFiles() const;
    void addRecentFile(const QString& path);
    void clearRecentFiles();

signals:
    void recentFilesChanged();

private:
    QString organisationKey() const;
};

}  // namespace alioth::app
