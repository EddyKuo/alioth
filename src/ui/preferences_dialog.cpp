#include "ui/preferences_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "app/settings.h"
#include "app/uisystem/cursor_scale.h"
#include "ui/shortcuts_page.h"

namespace alioth::ui {

PreferencesDialog::PreferencesDialog(app::Settings* settings, app::ShortcutScheme* scheme,
                                     QWidget* parent)
    : QDialog(parent), settings_(settings), originalLocale_(settings->locale()) {
    setWindowTitle(tr("偏好設定"));

    auto* form = new QFormLayout;

    cacheMb_ = new QSpinBox(this);
    // 範圍與 PRD §4.3 一致。用 MB 而不是位元組：沒有人想在設定框裡數零。
    cacheMb_->setRange(128, 1024);
    cacheMb_->setSingleStep(64);
    cacheMb_->setSuffix(tr(" MB"));
    cacheMb_->setValue(static_cast<int>(settings_->cacheBytes() / (1024 * 1024)));
    form->addRow(tr("圖磚快取上限"), cacheMb_);

    autosaveSeconds_ = new QSpinBox(this);
    autosaveSeconds_->setRange(0, 3600);
    autosaveSeconds_->setSingleStep(30);
    autosaveSeconds_->setSpecialValueText(tr("關閉"));
    autosaveSeconds_->setSuffix(tr(" 秒"));
    autosaveSeconds_->setValue(settings_->autosaveSeconds());
    form->addRow(tr("自動儲存間隔"), autosaveSeconds_);

    author_ = new QLineEdit(settings_->authorName(), this);
    author_->setPlaceholderText(tr("留空則使用系統使用者名稱"));
    form->addRow(tr("註解作者"), author_);

    nightMode_ = new QCheckBox(tr("啟用夜間模式"), this);
    nightMode_->setChecked(settings_->nightMode());
    form->addRow(QString(), nightMode_);

    // 觸控模式（PRD-UI-013）。做成開關而不是自動偵測：有觸控螢幕不代表使用者
    // 正在用手指——二合一筆電接著滑鼠時，自動放大按鈕只是把文件擠掉一截。
    touchMode_ = new QCheckBox(tr("觸控模式（放大按鈕命中區域）"), this);
    touchMode_->setObjectName(QStringLiteral("preferencesTouchMode"));
    touchMode_->setChecked(settings_->touchMode());
    form->addRow(QString(), touchMode_);

    // 可調整游標大小（PRD-UI-018）：系統的「指標大小」設定只會縮放系統內建
    // 游標，形狀工具用的十字準星是本程式自畫的，套用哪個尺寸得讓使用者選。
    cursorSize_ = new QComboBox(this);
    for (const app::CursorSizeLevel level :
         {app::CursorSizeLevel::Normal, app::CursorSizeLevel::Large,
          app::CursorSizeLevel::ExtraLarge, app::CursorSizeLevel::Huge}) {
        cursorSize_->addItem(app::CursorScale::displayName(level),
                             app::CursorScale::toSettingsValue(level));
    }
    cursorSize_->setCurrentIndex(
        cursorSize_->findData(app::CursorScale::toSettingsValue(settings_->cursorSizeLevel())));
    form->addRow(tr("游標大小"), cursorSize_);

    // 介面語言（PRD-UI-005）。改了要重新啟動才生效，所以在這裡就講清楚，
    // 而不是等使用者選完發現介面沒變、以為功能壞了。
    language_ = new QComboBox(this);
    language_->setObjectName(QStringLiteral("preferencesLanguage"));
    for (const app::Locale locale : app::availableLocales()) {
        language_->addItem(app::localeNativeName(locale), app::localeCode(locale));
    }
    language_->setCurrentIndex(language_->findData(app::localeCode(settings_->locale())));
    form->addRow(tr("介面語言"), language_);
    auto* languageHint = new QLabel(tr("變更語言後需重新啟動才會生效。"), this);
    languageHint->setWordWrap(true);
    form->addRow(QString(), languageHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 一般設定與快捷鍵分頁。快捷鍵那一頁夠大（一張表加六顆按鈕），塞進同一張
    // 表單會把真正常調的三個項目擠到要捲動才看得到的地方。
    auto* general = new QWidget(this);
    general->setObjectName(QStringLiteral("preferencesGeneralPage"));
    auto* generalLayout = new QVBoxLayout(general);
    generalLayout->setContentsMargins(0, 0, 0, 0);
    generalLayout->addLayout(form);
    generalLayout->addStretch();

    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("preferencesTabs"));
    tabs->addTab(general, tr("一般"));
    if (scheme != nullptr) {
        auto* shortcuts = new ShortcutsPage(scheme, this);
        tabs->addTab(shortcuts, tr("快捷鍵"));
        connect(shortcuts, &ShortcutsPage::schemeChanged, this,
                &PreferencesDialog::shortcutsChanged);
    }

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

void PreferencesDialog::apply() {
    settings_->setCacheBytes(static_cast<std::size_t>(cacheMb_->value()) * 1024 * 1024);
    settings_->setAutosaveSeconds(autosaveSeconds_->value());
    settings_->setAuthorName(author_->text());
    settings_->setNightMode(nightMode_->isChecked());
    settings_->setTouchMode(touchMode_->isChecked());
    settings_->setCursorSizeLevel(
        app::CursorScale::fromSettingsValue(cursorSize_->currentData().toInt()));
    settings_->setLocale(app::localeFromCode(language_->currentData().toString()));
}

bool PreferencesDialog::localeChanged() const {
    return app::localeFromCode(language_->currentData().toString()) != originalLocale_;
}

}  // namespace alioth::ui
