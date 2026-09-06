// 多語系測試（PRD-UI-005）。
//
// 三件事：語言清單、覆蓋率計算讀得到真的 .ts 檔、以及「缺翻譯退回原文」
// 這個假設被釘住（Qt QTranslator 的內建行為，見 locale_manager.h 開頭）。
// ALIOTH_I18N_SOURCE_DIR（.ts 來源，進版控）與 ALIOTH_I18N_BUILD_DIR（lrelease
// 編出來的 .qm，建置產物）由 CMake 分別注入，見 i18n/CMakeLists.txt。

#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <algorithm>

#include "app/uisystem/locale_manager.h"

using namespace alioth::app;

namespace {
QString i18nSourceDir() { return QStringLiteral(ALIOTH_I18N_SOURCE_DIR); }
QString i18nBuildDir() { return QStringLiteral(ALIOTH_I18N_BUILD_DIR); }
}  // namespace

class TestLocaleManager : public QObject {
    Q_OBJECT

private slots:
    void availableLocalesIncludesRequiredPair() {
        // PRD-UI-005：繁中、英必做。
        const auto locales = availableLocales();
        QVERIFY(std::find(locales.begin(), locales.end(), Locale::zh_TW) != locales.end());
        QVERIFY(std::find(locales.begin(), locales.end(), Locale::en) != locales.end());
    }

    void sourceLocaleCoverageIsAlwaysComplete() {
        QCOMPARE(translationCoverage(Locale::zh_TW, QStringLiteral("/nonexistent.ts")), 1.0);
    }

    void missingTsFileYieldsZeroCoverageNotCrash() {
        QCOMPARE(translationCoverage(Locale::en, QStringLiteral("/definitely/does/not/exist.ts")), 0.0);
    }

    void enTsFileHasNonZeroCoverage() {
        const QString tsPath = i18nSourceDir() + QStringLiteral("/alioth_en.ts");
        QVERIFY2(QFile::exists(tsPath), qPrintable(QStringLiteral("找不到 %1，i18n 產出流程未執行").arg(tsPath)));
        const double coverage = translationCoverage(Locale::en, tsPath);
        qInfo() << "en coverage:" << coverage;
        QVERIFY(coverage <= 1.0);
        // PRD-UI-005 把繁中與英文都列為必做，所以英文不是「有骨架就好」而是要全滿。
        //
        // 這條門檻同時擋住一種很難自己發現的腐化：新增 tr() 字串之後忘了跑
        // lupdate，.ts 停在舊的訊息集合上，覆蓋率看起來仍是 100%，而實際上
        // 介面已經有一半是中文。此處比對的是重新掃描過的 .ts，漏跑 lupdate
        // 會在下一次跑之後立刻紅。
        QVERIFY2(coverage == 1.0,
                 qPrintable(QStringLiteral("英文翻譯覆蓋率 %1，未達 100%——"
                                           "新增 tr() 字串後請重跑 lupdate 並補上翻譯")
                                .arg(coverage)));
    }

    void switchToSourceLocaleRemovesTranslatorAndFallsBackToOriginalText() {
        LocaleManager manager(qApp);
        QSignalSpy spy(&manager, &LocaleManager::localeChanged);
        const bool ok = manager.switchTo(Locale::zh_TW, i18nBuildDir());
        QVERIFY(ok);
        QCOMPARE(manager.current(), Locale::zh_TW);
        QCOMPARE(spy.count(), 1);
    }

    void switchToEnglishInstallsTranslatorWhenQmExists() {
        const QString qmPath = i18nBuildDir() + QStringLiteral("/alioth_en.qm");
        if (!QFile::exists(qmPath)) {
            QSKIP("alioth_en.qm 尚未由 lrelease 產生（見建置流程）");
        }
        LocaleManager manager(qApp);
        const bool ok = manager.switchTo(Locale::en, i18nBuildDir());
        QVERIFY(ok);
        QCOMPARE(manager.current(), Locale::en);
    }

    // 這支測試釘住的是 Qt 本身的契約，不是本專案重新實作的邏輯：
    // QCoreApplication::translate() 在找不到翻譯時回傳原始字面量本身，
    // 而不是空字串。LocaleManager 依賴這個契約達成「缺翻譯退回原文」，
    // 若哪天 Qt 改了這個行為，這支測試會先紅。
    void untranslatedStringFallsBackToSourceTextNotEmpty() {
        const QString sourceText = QStringLiteral("這個字串刻意不存在於任何 .ts 檔");
        // translate() 的第二參數是 const char*，Qt 的慣例是以來源檔案的編碼（本專案
        // 一律 UTF-8）解讀；用 qPrintable() 會先轉成主控台的本地編碼（Windows 上常是
        // Big5），字串在轉換過程就已經失真，不是 translate() 的問題。必須用
        // toUtf8().constData()，這也是 tr() 巨集實際展開後呼叫 translate() 的方式。
        const QByteArray utf8 = sourceText.toUtf8();
        const QString result = QCoreApplication::translate("TestLocaleManagerContext", utf8.constData());
        QCOMPARE(result, sourceText);
        QVERIFY(!result.isEmpty());
    }

    // 設定檔裡的語言碼由使用者可以手改，帶地區的寫法與完全看不懂的值都是常態。
    void localeCodeParsingAcceptsRegionSuffixAndFallsBackOnGarbage() {
        QCOMPARE(localeFromCode(QStringLiteral("en")), Locale::en);
        QCOMPARE(localeFromCode(QStringLiteral("EN")), Locale::en);
        QCOMPARE(localeFromCode(QStringLiteral("en_US")), Locale::en);
        QCOMPARE(localeFromCode(QStringLiteral("zh_TW")), Locale::zh_TW);
        // 尚未提供翻譯的語言與壞掉的值都落回原文，不是丟錯。
        QCOMPARE(localeFromCode(QStringLiteral("ja_JP")), Locale::zh_TW);
        QCOMPARE(localeFromCode(QString()), Locale::zh_TW);
        QCOMPARE(localeFromCode(QStringLiteral("!!!")), Locale::zh_TW);
    }

    // 每個語言碼都要能往返，否則存進 QSettings 再讀回來會換成別的語言。
    void everyLocaleRoundTripsThroughItsCode() {
        for (const Locale locale : availableLocales()) {
            QCOMPARE(localeFromCode(localeCode(locale)), locale);
            QVERIFY(!localeNativeName(locale).isEmpty());
        }
    }

    void translationsDirectoryIsAbsolute() {
        const QString dir = translationsDirectory();
        QVERIFY(!dir.isEmpty());
        QVERIFY(QDir::isAbsolutePath(dir));
    }
};

QTEST_MAIN(TestLocaleManager)
#include "test_locale_manager.moc"
