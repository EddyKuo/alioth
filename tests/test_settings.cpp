// 偏好設定與最近檔案。
//
// 設定的合法範圍寫錯不會崩潰，只會讓使用者設了一個看似生效、實際被忽略的值。
// 最近檔案清單則有一個很容易漏掉的要求：已刪除的檔案不該留在上面。

#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "app/settings.h"
#include "engine/tile_cache.h"

using namespace alioth;

class TestSettings : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // 測試不得污染使用者真正的設定，換一組 organisation/application 名稱。
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("SettingsTest"));
        QSettings().clear();
    }

    void init() { QSettings().clear(); }

    void cacheBytesIsClampedToSupportedRange() {
        app::Settings settings;
        QCOMPARE(settings.cacheBytes(), engine::kDefaultCacheBytes);

        settings.setCacheBytes(1);
        QCOMPARE(settings.cacheBytes(), engine::kMinCacheBytes);

        settings.setCacheBytes(64ull * 1024 * 1024 * 1024);
        QCOMPARE(settings.cacheBytes(), engine::kMaxCacheBytes);

        settings.setCacheBytes(512ull * 1024 * 1024);
        QCOMPARE(settings.cacheBytes(), 512ull * 1024 * 1024);
    }

    void autosaveIntervalRejectsUselesslyShortValues() {
        app::Settings settings;
        QCOMPARE(settings.autosaveSeconds(), 120);

        // 關閉是使用者的權利。
        settings.setAutosaveSeconds(0);
        QCOMPARE(settings.autosaveSeconds(), 0);

        // 但每秒存一次會讓大型文件永遠在寫檔，所以有下限。
        settings.setAutosaveSeconds(1);
        QCOMPARE(settings.autosaveSeconds(), 30);

        settings.setAutosaveSeconds(-5);
        QCOMPARE(settings.autosaveSeconds(), 0);
    }

    void recentFilesAreOrderedAndDeduplicated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto makeFile = [&dir](const QString& name) {
            const QString path = dir.filePath(name);
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write("%PDF-1.7\n");
            file.close();
            return path;
        };

        const QString a = makeFile(QStringLiteral("a.pdf"));
        const QString b = makeFile(QStringLiteral("b.pdf"));

        app::Settings settings;
        QSignalSpy changed(&settings, &app::Settings::recentFilesChanged);

        settings.addRecentFile(a);
        settings.addRecentFile(b);
        QCOMPARE(settings.recentFiles().size(), 2);
        QCOMPARE(settings.recentFiles().front(), b);

        // 重開舊檔只是把它移到最前，不該出現兩次。
        settings.addRecentFile(a);
        QCOMPARE(settings.recentFiles().size(), 2);
        QCOMPARE(settings.recentFiles().front(), a);
        QCOMPARE(changed.count(), 3);
    }

    void recentFilesDropEntriesThatNoLongerExist() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("gone.pdf"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("%PDF-1.7\n");
        }

        app::Settings settings;
        settings.addRecentFile(path);
        QCOMPARE(settings.recentFiles().size(), 1);

        QVERIFY(QFile::remove(path));
        // 點下去只會得到錯誤對話框的項目不該留在清單上。
        QVERIFY(settings.recentFiles().isEmpty());
    }

    void recentFilesAreCapped() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        app::Settings settings;

        for (int i = 0; i < app::Settings::kMaxRecentFiles + 5; ++i) {
            const QString path = dir.filePath(QStringLiteral("f%1.pdf").arg(i));
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write("%PDF-1.7\n");
            file.close();
            settings.addRecentFile(path);
        }
        QCOMPARE(settings.recentFiles().size(), app::Settings::kMaxRecentFiles);
    }

    void emptyPathIsIgnored() {
        app::Settings settings;
        settings.addRecentFile(QString());
        QVERIFY(settings.recentFiles().isEmpty());
    }

    void authorAndNightModeRoundTrip() {
        app::Settings settings;
        QVERIFY(settings.authorName().isEmpty());
        settings.setAuthorName(QStringLiteral("審閱者"));
        QCOMPARE(settings.authorName(), QStringLiteral("審閱者"));

        QVERIFY(!settings.nightMode());
        settings.setNightMode(true);
        QVERIFY(settings.nightMode());
    }

    // 介面語言（PRD-UI-005）。沒設定過時跟隨系統語言——不是硬鎖繁中，
    // 也不是空值，否則第一次啟動的英文系統使用者會看到看不懂的介面。
    void localeDefaultsToSystemAndRoundTrips() {
        app::Settings settings;
        QCOMPARE(settings.locale(), app::systemLocale());

        settings.setLocale(app::Locale::en);
        QCOMPARE(settings.locale(), app::Locale::en);
        settings.setLocale(app::Locale::zh_TW);
        QCOMPARE(settings.locale(), app::Locale::zh_TW);
    }
};

QTEST_MAIN(TestSettings)
#include "test_settings.moc"
