// 設定檔匯出／匯入／重設（PRD-UI-014）與企業部署鎖定（PRD-UI-015）。
//
// 這裡的判準都圍繞同一件事：**使用者要能分辨「套用了」與「沒套用」**。
// 一份設定檔只套用了一半而沒人知道，比整份失敗糟糕得多。

#include <QtTest>

#include <QSettings>

#include "app/settings.h"
#include "app/settings_profile.h"

using namespace alioth;
using app::ShortcutScheme;
using app::ThemeMode;

class TestSettingsProfile : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // 測試不得污染使用者真正的設定，也不能被它污染——本檔第一版就是因為
        // 讀到機器上既有的 autosave 值而失敗，症狀看起來像「套用沒生效」。
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("SettingsProfileTest"));
        QSettings().clear();
    }

    void init() { QSettings().clear(); }

    void roundTripPreservesEverything() {
        app::SettingsProfile profile;
        profile.name = QStringLiteral("工程部標準設定");
        profile.description = QStringLiteral("含快捷鍵與主題");
        profile.cacheBytes = 512u * 1024u * 1024u;
        profile.autosaveSeconds = 120;
        profile.authorName = QStringLiteral("Eddy");
        profile.nightMode = true;
        profile.themeMode = ThemeMode::Dark;
        ShortcutScheme scheme;
        profile.shortcutsJson = scheme.exportToJson();

        app::SettingsProfile restored;
        QString diagnostic;
        QVERIFY2(app::parseProfile(app::exportProfile(profile), restored, &diagnostic),
                 qPrintable(diagnostic));

        QCOMPARE(restored.name, profile.name);
        QCOMPARE(restored.description, profile.description);
        QCOMPARE(restored.cacheBytes, profile.cacheBytes);
        QCOMPARE(restored.autosaveSeconds, 120);
        QCOMPARE(restored.authorName, QStringLiteral("Eddy"));
        QCOMPARE(*restored.nightMode, true);
        QCOMPARE(*restored.themeMode, ThemeMode::Dark);
        QVERIFY(!restored.shortcutsJson.isEmpty());
    }

    void exportedFileIsHumanReadableJson() {
        // 嵌進去的快捷鍵表要是真的 JSON 結構，不是被塞成一整條字串——
        // 塞字串會讓整份設定檔沒辦法用一般工具處理或人工檢視。
        app::SettingsProfile profile;
        ShortcutScheme scheme;
        profile.shortcutsJson = scheme.exportToJson();

        const QByteArray json = app::exportProfile(profile);
        const QJsonDocument document = QJsonDocument::fromJson(json);
        QVERIFY(document.isObject());
        const QJsonValue shortcuts = document.object().value(QStringLiteral("shortcuts"));
        QVERIFY(shortcuts.isArray() || shortcuts.isObject());
        QVERIFY(!shortcuts.isString());
    }

    void newerVersionIsRejectedRatherThanPartiallyRead() {
        // 硬讀會把不認得的欄位靜靜丟掉，使用者卻以為整份都套用了。
        const QByteArray json = QByteArrayLiteral("{\"version\":99,\"cacheBytes\":1}");
        app::SettingsProfile profile;
        QString diagnostic;
        QVERIFY(!app::parseProfile(json, profile, &diagnostic));
        QVERIFY(diagnostic.contains(QStringLiteral("99")));
    }

    void malformedJsonFailsWithAReason() {
        app::SettingsProfile profile;
        QString diagnostic;
        QVERIFY(!app::parseProfile(QByteArrayLiteral("{ not json"), profile, &diagnostic));
        QVERIFY(!diagnostic.isEmpty());

        QVERIFY(!app::parseProfile(QByteArrayLiteral("[1,2,3]"), profile, &diagnostic));
        QVERIFY(!app::parseProfile(QByteArrayLiteral("{\"cacheBytes\":1}"), profile, &diagnostic));
    }

    void unknownThemeValueIsRejected() {
        // 猜成 system 會讓使用者以為設定檔套用成功了。
        app::SettingsProfile profile;
        QString diagnostic;
        QVERIFY(!app::parseProfile(QByteArrayLiteral("{\"version\":1,\"theme\":\"neon\"}"), profile,
                                   &diagnostic));
        QVERIFY(!diagnostic.isEmpty());
    }

    void personalTraceIsNotCaptured() {
        // 最近檔案與文件歷史不進設定檔：跟著它散出去等於洩漏使用者看過哪些文件。
        app::Settings settings;
        settings.addRecentFile(QStringLiteral("C:/secret/merger-plan.pdf"));
        ShortcutScheme scheme;

        const QByteArray json =
            app::exportProfile(app::captureProfile(settings, scheme, ThemeMode::Light));
        QVERIFY(!json.contains("merger-plan"));
        QVERIFY(!json.contains("recent"));
    }

    void lockedKeysAreReportedAsSkippedNotApplied() {
        app::SettingsProfile profile;
        profile.version = 1;
        profile.cacheBytes = 512u * 1024u * 1024u;
        profile.autosaveSeconds = 60;
        profile.themeMode = ThemeMode::Dark;
        profile.lockedKeys = {QStringLiteral("theme"), QStringLiteral("autosave")};

        app::Settings settings;
        ShortcutScheme scheme;
        ThemeMode theme = ThemeMode::Light;
        const app::ProfileImportResult result =
            app::applyProfile(profile, settings, scheme, &theme);

        QVERIFY(result.ok);
        QVERIFY(result.appliedKeys.contains(QStringLiteral("cache")));
        QVERIFY(result.skippedKeys.contains(QStringLiteral("theme")));
        QVERIFY(result.skippedKeys.contains(QStringLiteral("autosave")));
        // 鎖定的項目不可以被改到。
        QCOMPARE(theme, ThemeMode::Light);
    }

    void appliedValuesActuallyReachTheSettings() {
        app::SettingsProfile profile;
        profile.version = 1;
        profile.cacheBytes = 384u * 1024u * 1024u;
        profile.autosaveSeconds = 45;
        profile.authorName = QStringLiteral("部門範本");

        app::Settings settings;
        ShortcutScheme scheme;
        ThemeMode theme = ThemeMode::System;
        QVERIFY(app::applyProfile(profile, settings, scheme, &theme).ok);

        QCOMPARE(settings.autosaveSeconds(), 45);
        QCOMPARE(settings.authorName(), QStringLiteral("部門範本"));
    }

    void badShortcutTableDoesNotFailTheWholeImport() {
        // 其餘項目已經套上去了。整份回報失敗會讓使用者以為什麼都沒發生。
        app::SettingsProfile profile;
        profile.version = 1;
        profile.cacheBytes = 300u * 1024u * 1024u;
        profile.shortcutsJson = QByteArrayLiteral("{ broken");

        app::Settings settings;
        ShortcutScheme scheme;
        ThemeMode theme = ThemeMode::System;
        const app::ProfileImportResult result =
            app::applyProfile(profile, settings, scheme, &theme);

        QVERIFY(result.ok);
        QVERIFY(result.appliedKeys.contains(QStringLiteral("cache")));
        QVERIFY(result.skippedKeys.contains(QStringLiteral("shortcuts")));
        QVERIFY(!result.diagnostic.isEmpty());
    }

    void resetReportsWhatItChanged() {
        // 「重設」按下去畫面沒變化時，使用者無法分辨是成功了還是壞了。
        app::Settings settings;
        settings.setAutosaveSeconds(11);
        ShortcutScheme scheme;
        ThemeMode theme = ThemeMode::Dark;

        const QStringList reset = app::resetToFactoryDefaults(settings, scheme, &theme);
        QVERIFY(!reset.isEmpty());
        QVERIFY(reset.contains(QStringLiteral("shortcuts")));
        QCOMPARE(theme, ThemeMode::System);
        QCOMPARE(settings.autosaveSeconds(), 300);
    }

    void resetKeepsTheAuthorName() {
        // 清掉作者名稱會讓之後所有新註解變成匿名，而使用者不會想到是重設造成的。
        app::Settings settings;
        settings.setAuthorName(QStringLiteral("Eddy"));
        ShortcutScheme scheme;
        ThemeMode theme = ThemeMode::Light;

        app::resetToFactoryDefaults(settings, scheme, &theme);
        QCOMPARE(settings.authorName(), QStringLiteral("Eddy"));
    }
};

QTEST_MAIN(TestSettingsProfile)
#include "test_settings_profile.moc"
