#include "app/settings.h"

#include <QFileInfo>
#include <QSettings>

#include <algorithm>

#include "engine/tile_cache.h"

namespace alioth::app {
namespace {

constexpr const char* kCacheBytesKey = "render/cacheBytes";
constexpr const char* kAutosaveKey = "io/autosaveSeconds";
constexpr const char* kAuthorKey = "annotation/author";
constexpr const char* kNightModeKey = "view/nightMode";
constexpr const char* kRecentKey = "io/recentFiles";
constexpr const char* kCursorSizeKey = "view/cursorSizeLevel";
constexpr const char* kLocaleKey = "ui/locale";
constexpr const char* kStickyToolsKey = "ui/stickyTools";
constexpr const char* kShortcutsKey = "ui/shortcuts";
constexpr const char* kTouchModeKey = "ui/touchMode";
constexpr const char* kExternalToolsGroup = "externalTools";

}  // namespace

Settings::Settings(QObject* parent) : QObject(parent) {}

std::size_t Settings::cacheBytes() const {
    QSettings settings;
    const auto stored = settings.value(kCacheBytesKey,
                                       static_cast<qulonglong>(engine::kDefaultCacheBytes))
                            .toULongLong();
    return std::clamp<std::size_t>(static_cast<std::size_t>(stored), engine::kMinCacheBytes,
                                   engine::kMaxCacheBytes);
}

void Settings::setCacheBytes(std::size_t bytes) {
    QSettings settings;
    const std::size_t clamped =
        std::clamp<std::size_t>(bytes, engine::kMinCacheBytes, engine::kMaxCacheBytes);
    settings.setValue(kCacheBytesKey, static_cast<qulonglong>(clamped));
}

int Settings::autosaveSeconds() const {
    QSettings settings;
    const int stored = settings.value(kAutosaveKey, 120).toInt();
    // 負值與過短的間隔都當成無效：每秒自動儲存一次會讓大型文件永遠在寫檔。
    if (stored <= 0) return 0;
    return std::max(30, stored);
}

void Settings::setAutosaveSeconds(int seconds) {
    QSettings settings;
    settings.setValue(kAutosaveKey, seconds <= 0 ? 0 : std::max(30, seconds));
}

QString Settings::authorName() const {
    QSettings settings;
    return settings.value(kAuthorKey).toString();
}

void Settings::setAuthorName(const QString& name) {
    QSettings settings;
    settings.setValue(kAuthorKey, name);
}

bool Settings::nightMode() const {
    QSettings settings;
    return settings.value(kNightModeKey, false).toBool();
}

void Settings::setNightMode(bool enabled) {
    QSettings settings;
    settings.setValue(kNightModeKey, enabled);
}

CursorSizeLevel Settings::cursorSizeLevel() const {
    QSettings settings;
    return CursorScale::fromSettingsValue(settings.value(kCursorSizeKey, 0).toInt());
}

void Settings::setCursorSizeLevel(CursorSizeLevel level) {
    QSettings settings;
    settings.setValue(kCursorSizeKey, CursorScale::toSettingsValue(level));
}

bool Settings::stickyTools() const {
    QSettings settings;
    return settings.value(kStickyToolsKey, false).toBool();
}

void Settings::setStickyTools(bool enabled) {
    QSettings settings;
    settings.setValue(kStickyToolsKey, enabled);
}

bool Settings::touchMode() const {
    QSettings settings;
    return settings.value(kTouchModeKey, false).toBool();
}

void Settings::setTouchMode(bool enabled) {
    QSettings settings;
    settings.setValue(kTouchModeKey, enabled);
}

QByteArray Settings::shortcutsJson() const {
    QSettings settings;
    return settings.value(kShortcutsKey).toByteArray();
}

void Settings::setShortcutsJson(const QByteArray& json) {
    QSettings settings;
    // 空字串代表「沒有自訂過」，明確移除比存一個空值好——存空值的話下次讀出來
    // 是空 JSON，importFromJson 會判成格式錯誤而不是「沒有自訂」。
    if (json.isEmpty()) {
        settings.remove(kShortcutsKey);
        return;
    }
    settings.setValue(kShortcutsKey, json);
}

Locale Settings::locale() const {
    QSettings settings;
    const QString code = settings.value(kLocaleKey).toString();
    // 沒有設定過時跟隨系統語言，而不是硬鎖繁中：第一次啟動就看到看不懂的介面，
    // 使用者未必找得到偏好設定裡的語言選項。
    if (code.isEmpty()) return systemLocale();
    return localeFromCode(code);
}

void Settings::setLocale(Locale locale) {
    QSettings settings;
    settings.setValue(kLocaleKey, localeCode(locale));
}

std::vector<ExternalTool> Settings::externalTools() const {
    QSettings settings;
    std::vector<ExternalTool> tools;
    const int count = settings.beginReadArray(kExternalToolsGroup);
    tools.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        ExternalTool tool;
        tool.name = settings.value(QStringLiteral("name")).toString();
        tool.executablePath = settings.value(QStringLiteral("executablePath")).toString();
        tool.argumentsTemplate = settings.value(QStringLiteral("argumentsTemplate")).toString();
        tool.workingDirectory = settings.value(QStringLiteral("workingDirectory")).toString();
        tools.push_back(tool);
    }
    settings.endArray();
    return tools;
}

void Settings::setExternalTools(const std::vector<ExternalTool>& tools) {
    QSettings settings;
    settings.beginWriteArray(kExternalToolsGroup);
    for (std::size_t i = 0; i < tools.size(); ++i) {
        settings.setArrayIndex(static_cast<int>(i));
        settings.setValue(QStringLiteral("name"), tools[i].name);
        settings.setValue(QStringLiteral("executablePath"), tools[i].executablePath);
        settings.setValue(QStringLiteral("argumentsTemplate"), tools[i].argumentsTemplate);
        settings.setValue(QStringLiteral("workingDirectory"), tools[i].workingDirectory);
    }
    settings.endArray();
}

QStringList Settings::recentFiles() const {
    QSettings settings;
    QStringList files = settings.value(kRecentKey).toStringList();

    // 已經不存在的檔案不該留在清單上：使用者點下去只會得到一個錯誤對話框。
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const QString& path) { return !QFileInfo::exists(path); }),
                files.end());
    return files;
}

void Settings::addRecentFile(const QString& path) {
    if (path.isEmpty()) return;

    const QString absolute = QFileInfo(path).absoluteFilePath();
    QStringList files = recentFiles();
    files.removeAll(absolute);
    files.prepend(absolute);
    while (files.size() > kMaxRecentFiles) {
        files.removeLast();
    }

    QSettings settings;
    settings.setValue(kRecentKey, files);
    emit recentFilesChanged();
}

void Settings::clearRecentFiles() {
    QSettings settings;
    settings.remove(kRecentKey);
    emit recentFilesChanged();
}

}  // namespace alioth::app
