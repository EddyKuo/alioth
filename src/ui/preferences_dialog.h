#pragma once

// 偏好設定對話框（PRD-UI-008）。
//
// 只放真正會影響行為的項目。設定畫面塞滿沒人動的開關，代價是使用者再也找不到
// 那三個真的需要調的東西。

#include <QDialog>

#include "app/uisystem/locale_manager.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace alioth::app {
class Settings;
class ShortcutScheme;
}

namespace alioth::ui {

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    // scheme 為 nullptr 時不顯示快捷鍵分頁——設定對話框在沒有鍵位表的情境
    // （例如單元測試）仍要能開起來。
    PreferencesDialog(app::Settings* settings, app::ShortcutScheme* scheme = nullptr,
                      QWidget* parent = nullptr);

    // 按下確定後把值寫回 Settings。呼叫端負責把新值套用到執行中的元件——
    // 對話框不直接碰控制器，才不會變成第二個 main_window。
    void apply();

    // 語言與開啟對話框時不同。呼叫端據此提示需要重新啟動——由呼叫端提示而不是
    // 對話框自己跳訊息，是因為「確定」之後還會套用其他設定，兩個視窗疊在一起
    // 只會讓使用者搞不清楚剛才按的是哪一個。
    [[nodiscard]] bool localeChanged() const;

signals:
    // 快捷鍵在對話框還開著的時候就變了。鍵位是即時套用的：使用者改完一個鍵
    // 之後按取消，期望的是「剛才那個改動還在」而不是整頁一起回滾——鍵位表
    // 有自己的還原預設按鈕，不需要靠對話框的取消鍵扮演第二個復原機制。
    void shortcutsChanged();

private:
    app::Settings* settings_{nullptr};
    QSpinBox* cacheMb_{nullptr};
    QSpinBox* autosaveSeconds_{nullptr};
    QLineEdit* author_{nullptr};
    QCheckBox* nightMode_{nullptr};
    QCheckBox* touchMode_{nullptr};  // PRD-UI-013：觸控模式
    QComboBox* cursorSize_{nullptr};  // PRD-UI-018：可調整游標大小
    QComboBox* language_{nullptr};    // PRD-UI-005：介面語言
    app::Locale originalLocale_{};
};

}  // namespace alioth::ui
