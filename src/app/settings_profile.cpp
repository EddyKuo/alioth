#include "app/settings_profile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "app/settings.h"

namespace alioth::app {
namespace {

constexpr int kCurrentVersion = 1;

QString themeToString(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:  return QStringLiteral("light");
        case ThemeMode::Dark:   return QStringLiteral("dark");
        case ThemeMode::System: break;
    }
    return QStringLiteral("system");
}

std::optional<ThemeMode> themeFromString(const QString& text) {
    if (text == QStringLiteral("light")) return ThemeMode::Light;
    if (text == QStringLiteral("dark")) return ThemeMode::Dark;
    if (text == QStringLiteral("system")) return ThemeMode::System;
    // 認不得的值不猜。猜成 system 會讓使用者以為設定檔套用成功了。
    return std::nullopt;
}

}  // namespace

SettingsProfile captureProfile(const Settings& settings, const ShortcutScheme& shortcuts,
                               ThemeMode theme, const QString& name) {
    SettingsProfile profile;
    profile.version = kCurrentVersion;
    profile.name = name;
    profile.cacheBytes = settings.cacheBytes();
    profile.autosaveSeconds = settings.autosaveSeconds();
    profile.authorName = settings.authorName();
    profile.nightMode = settings.nightMode();
    profile.themeMode = theme;
    profile.shortcutsJson = shortcuts.exportToJson();
    // 最近檔案與文件歷史刻意不收：那是個人足跡，跟著設定檔散出去等於洩漏
    // 使用者看過哪些文件。
    return profile;
}

QByteArray exportProfile(const SettingsProfile& profile) {
    QJsonObject root;
    root[QStringLiteral("version")] = profile.version;
    if (!profile.name.isEmpty()) root[QStringLiteral("name")] = profile.name;
    if (!profile.description.isEmpty()) root[QStringLiteral("description")] = profile.description;
    root[QStringLiteral("cacheBytes")] = static_cast<double>(profile.cacheBytes);
    root[QStringLiteral("autosaveSeconds")] = profile.autosaveSeconds;
    if (!profile.authorName.isEmpty()) root[QStringLiteral("authorName")] = profile.authorName;
    if (profile.nightMode.has_value()) root[QStringLiteral("nightMode")] = *profile.nightMode;
    if (profile.themeMode.has_value()) {
        root[QStringLiteral("theme")] = themeToString(*profile.themeMode);
    }

    // 快捷鍵與 Ribbon 配置各自已經是 JSON。用 QJsonDocument 解回物件再嵌入，
    // 而不是塞成字串——塞字串會讓整份設定檔沒辦法被人工閱讀或用一般工具處理。
    if (!profile.shortcutsJson.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(profile.shortcutsJson);
        if (!document.isNull()) {
            root[QStringLiteral("shortcuts")] =
                document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
        }
    }
    if (!profile.ribbonLayoutJson.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(profile.ribbonLayoutJson);
        if (!document.isNull()) {
            root[QStringLiteral("ribbon")] =
                document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
        }
    }
    if (!profile.lockedKeys.isEmpty()) {
        QJsonArray locked;
        for (const QString& key : profile.lockedKeys) locked.append(key);
        root[QStringLiteral("locked")] = locked;
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool parseProfile(const QByteArray& json, SettingsProfile& out, QString* diagnostic) {
    const auto fail = [diagnostic](const QString& message) {
        if (diagnostic != nullptr) *diagnostic = message;
        return false;
    };

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        return fail(QObject::tr("JSON 格式錯誤：%1").arg(error.errorString()));
    }
    if (!document.isObject()) return fail(QObject::tr("設定檔的最外層必須是物件"));

    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt(0);
    if (version <= 0) return fail(QObject::tr("設定檔缺少版本號"));
    if (version > kCurrentVersion) {
        // 比程式新的設定檔明確拒絕。硬讀會把不認得的欄位靜靜丟掉，
        // 而使用者會以為整份都套用了。
        return fail(QObject::tr("設定檔版本 %1 比本程式支援的 %2 新")
                        .arg(version)
                        .arg(kCurrentVersion));
    }

    SettingsProfile profile;
    profile.version = version;
    profile.name = root.value(QStringLiteral("name")).toString();
    profile.description = root.value(QStringLiteral("description")).toString();
    profile.cacheBytes = static_cast<std::size_t>(
        root.value(QStringLiteral("cacheBytes")).toDouble(0.0));
    profile.autosaveSeconds = root.value(QStringLiteral("autosaveSeconds")).toInt(-1);
    profile.authorName = root.value(QStringLiteral("authorName")).toString();
    if (root.contains(QStringLiteral("nightMode"))) {
        profile.nightMode = root.value(QStringLiteral("nightMode")).toBool();
    }
    if (root.contains(QStringLiteral("theme"))) {
        profile.themeMode = themeFromString(root.value(QStringLiteral("theme")).toString());
        if (!profile.themeMode.has_value()) {
            return fail(QObject::tr("認不得的主題設定值"));
        }
    }
    if (root.contains(QStringLiteral("shortcuts"))) {
        const QJsonValue value = root.value(QStringLiteral("shortcuts"));
        profile.shortcutsJson = value.isArray()
                                    ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                    : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    }
    if (root.contains(QStringLiteral("ribbon"))) {
        const QJsonValue value = root.value(QStringLiteral("ribbon"));
        profile.ribbonLayoutJson =
            value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                            : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    }
    for (const QJsonValue& value : root.value(QStringLiteral("locked")).toArray()) {
        if (value.isString()) profile.lockedKeys.append(value.toString());
    }

    out = std::move(profile);
    return true;
}

ProfileImportResult applyProfile(const SettingsProfile& profile, Settings& settings,
                                 ShortcutScheme& shortcuts, ThemeMode* themeOut) {
    ProfileImportResult result;

    const auto record = [&result, &profile](const QString& key, bool applied) {
        if (profile.isLocked(key)) {
            result.skippedKeys.append(key);
            return false;
        }
        if (applied) result.appliedKeys.append(key);
        return applied;
    };

    if (!profile.isLocked(QStringLiteral("cache")) && profile.cacheBytes > 0) {
        settings.setCacheBytes(profile.cacheBytes);
        result.appliedKeys.append(QStringLiteral("cache"));
    } else if (profile.isLocked(QStringLiteral("cache"))) {
        result.skippedKeys.append(QStringLiteral("cache"));
    }

    if (!profile.isLocked(QStringLiteral("autosave")) && profile.autosaveSeconds >= 0) {
        settings.setAutosaveSeconds(profile.autosaveSeconds);
        result.appliedKeys.append(QStringLiteral("autosave"));
    } else if (profile.isLocked(QStringLiteral("autosave"))) {
        result.skippedKeys.append(QStringLiteral("autosave"));
    }

    if (!profile.authorName.isEmpty()) {
        record(QStringLiteral("author"), true);
        if (!profile.isLocked(QStringLiteral("author"))) {
            settings.setAuthorName(profile.authorName);
        }
    }

    if (profile.nightMode.has_value() && !profile.isLocked(QStringLiteral("nightMode"))) {
        settings.setNightMode(*profile.nightMode);
        result.appliedKeys.append(QStringLiteral("nightMode"));
    }

    if (profile.themeMode.has_value()) {
        if (profile.isLocked(QStringLiteral("theme"))) {
            result.skippedKeys.append(QStringLiteral("theme"));
        } else if (themeOut != nullptr) {
            *themeOut = *profile.themeMode;
            result.appliedKeys.append(QStringLiteral("theme"));
        }
    }

    if (!profile.shortcutsJson.isEmpty()) {
        if (profile.isLocked(QStringLiteral("shortcuts"))) {
            result.skippedKeys.append(QStringLiteral("shortcuts"));
        } else {
            const ShortcutScheme::ImportResult imported =
                shortcuts.importFromJson(profile.shortcutsJson);
            if (imported.ok) {
                result.appliedKeys.append(QStringLiteral("shortcuts"));
            } else {
                // 鍵位表匯入失敗不該讓整份設定檔失敗——其餘項目已經套上去了，
                // 回報失敗會讓使用者以為什麼都沒發生然後再試一次。
                result.skippedKeys.append(QStringLiteral("shortcuts"));
                result.diagnostic = imported.error.isEmpty()
                                        ? QObject::tr("快捷鍵表有衝突，未套用")
                                        : imported.error;
            }
        }
    }

    result.ok = true;
    return result;
}

QStringList resetToFactoryDefaults(Settings& settings, ShortcutScheme& shortcuts,
                                   ThemeMode* themeOut) {
    QStringList reset;

    // 這些預設值與 Settings 建構時的預設一致。重複一份的理由是「重設」
    // 必須是明確的動作，而不是「把 QSettings 刪掉再賭它會長回來」——
    // 後者會連同使用者的其他鍵一起消失。
    settings.setCacheBytes(256u * 1024u * 1024u);
    reset.append(QStringLiteral("cache"));
    settings.setAutosaveSeconds(300);
    reset.append(QStringLiteral("autosave"));
    settings.setNightMode(false);
    reset.append(QStringLiteral("nightMode"));

    shortcuts.resetToDefaults();
    reset.append(QStringLiteral("shortcuts"));

    if (themeOut != nullptr) {
        *themeOut = ThemeMode::System;
        reset.append(QStringLiteral("theme"));
    }

    // 作者名稱不重設：那是使用者填的個人資料，把它清掉之後所有新註解
    // 都會變成匿名，而使用者不會想到是「重設設定」造成的。
    return reset;
}

}  // namespace alioth::app
