#include "app/uisystem/locale_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QTranslator>
#include <QXmlStreamReader>

namespace alioth::app {

QString localeCode(Locale locale) {
    switch (locale) {
        case Locale::zh_TW: return QStringLiteral("zh_TW");
        case Locale::en: return QStringLiteral("en");
    }
    return QStringLiteral("zh_TW");
}

QString localeNativeName(Locale locale) {
    switch (locale) {
        case Locale::zh_TW: return QStringLiteral("繁體中文");
        case Locale::en: return QStringLiteral("English");
    }
    return QStringLiteral("繁體中文");
}

std::vector<Locale> availableLocales() { return {Locale::zh_TW, Locale::en}; }

Locale localeFromCode(const QString& code) {
    for (const Locale locale : availableLocales()) {
        if (localeCode(locale).compare(code, Qt::CaseInsensitive) == 0) return locale;
    }
    // 只比對語言部分：設定檔可能寫著 "en_US" 或 "zh_Hant_TW"，那些都該對得上。
    const QString language = code.left(code.indexOf(QLatin1Char('_')) < 0
                                           ? code.size()
                                           : code.indexOf(QLatin1Char('_')));
    if (language.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) return Locale::en;
    return Locale::zh_TW;
}

Locale systemLocale() {
    // 只有明確是英文系統才用英文。其餘（含簡中、日文等尚未提供翻譯的語言）
    // 一律落回繁中原文——顯示原文比顯示一個空殼英文介面誠實。
    if (QLocale::system().language() == QLocale::English) return Locale::en;
    return Locale::zh_TW;
}

QString translationsDirectory() {
    // 安裝後 .qm 就在執行檔旁的 i18n/；開發時建置輸出目錄的 i18n/ 也在同一層
    // （見 i18n/CMakeLists.txt 把 .qm 寫到 ${CMAKE_BINARY_DIR}/i18n）。
    // 執行檔在建置目錄的更深一層（例如 out/build/.../src/），所以往上找兩層。
    const QString base = QCoreApplication::applicationDirPath();
    for (const QString& candidate : {base + QStringLiteral("/i18n"),
                                     base + QStringLiteral("/../i18n"),
                                     base + QStringLiteral("/../../i18n")}) {
        if (QDir(candidate).exists()) return QDir(candidate).absolutePath();
    }
    return base + QStringLiteral("/i18n");
}

double translationCoverage(Locale locale, const QString& tsFilePath) {
    if (locale == Locale::zh_TW) return 1.0;  // 原始語言就是它自己

    QFile file(tsFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return 0.0;

    QXmlStreamReader xml(&file);
    long long total = 0;
    long long translated = 0;
    bool inMessage = false;
    bool currentIsUnfinished = false;
    bool currentHasText = false;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == QStringLiteral("message")) {
                inMessage = true;
                currentIsUnfinished = false;
                currentHasText = false;
            } else if (inMessage && xml.name() == QStringLiteral("translation")) {
                const QString type = xml.attributes().value(QStringLiteral("type")).toString();
                currentIsUnfinished = (type == QStringLiteral("unfinished") ||
                                       type == QStringLiteral("obsolete") ||
                                       type == QStringLiteral("vanished"));
                const QString text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                currentHasText = !text.trimmed().isEmpty();
            }
        } else if (token == QXmlStreamReader::EndElement && xml.name() == QStringLiteral("message")) {
            ++total;
            if (!currentIsUnfinished && currentHasText) ++translated;
            inMessage = false;
        }
    }
    if (total == 0) return 0.0;
    return static_cast<double>(translated) / static_cast<double>(total);
}

LocaleManager::LocaleManager(QCoreApplication* app, QObject* parent) : QObject(parent), app_(app) {}

LocaleManager::~LocaleManager() {
    if (translator_ && app_) app_->removeTranslator(translator_);
    delete translator_;
}

bool LocaleManager::switchTo(Locale locale, const QString& qmDirectory) {
    if (translator_ && app_) {
        app_->removeTranslator(translator_);
        delete translator_;
        translator_ = nullptr;
    }

    bool ok = true;
    if (locale != Locale::zh_TW) {
        translator_ = new QTranslator(this);
        const QString qmFile = QStringLiteral("alioth_%1").arg(localeCode(locale));
        ok = translator_->load(qmFile, qmDirectory);
        if (ok && app_) {
            app_->installTranslator(translator_);
        } else if (!ok) {
            delete translator_;
            translator_ = nullptr;
        }
    }
    // zh_TW 或載入失敗時都會落回「沒有安裝翻譯器」＝ tr() 回傳原始字面量，
    // 這正是「缺翻譯時退回原文」的機制本身，不需要另外寫退回邏輯。
    current_ = locale;
    emit localeChanged(current_);
    return ok;
}

}  // namespace alioth::app
