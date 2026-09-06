#pragma once

// 多語系（PRD-UI-005）：繁中為原始語言（程式碼裡的 tr() 字面量即繁中），
// 英文為翻譯目標，走 Qt 標準的 .ts/.qm 流程（見 i18n/CMakeLists.txt）。
//
// LocaleManager 負責三件事：
//   1. 列出可用語言、回報每個語言的翻譯覆蓋率（供偏好設定 UI 與本次任務報告用）
//   2. 切換語言：安裝／移除 QTranslator，並發出 localeChanged 讓呈現層重建
//      （Qt 的慣例是收到 QEvent::LanguageChange 後在 changeEvent() 裡呼叫
//      retranslateUi 之類的重建；本專案目前 widget 尚未逐一補上 retranslate，
//      見任務報告的已知缺口）
//   3. 保證「缺翻譯時退回原文」：這是 Qt QTranslator 的內建行為
//      （找不到翻譯時 tr() 回傳原始字面量），這裡用一支測試釘住這個假設，
//      不是重新實作它。

#include <QObject>
#include <QString>

#include <vector>

class QTranslator;
class QCoreApplication;

namespace alioth::app {

enum class Locale {
    zh_TW,  // 原始語言，永遠 100% 覆蓋（就是原始字面量本身）
    en,
};

QString localeCode(Locale locale);     // "zh_TW" / "en"
QString localeNativeName(Locale locale);  // "繁體中文" / "English"
std::vector<Locale> availableLocales();

// 設定檔或系統回報的語言碼 → Locale。認得 "en_US" 這種帶地區的寫法；
// 不認得的一律落回 zh_TW（原文），而不是丟錯——語言碼壞掉不該讓程式起不來。
Locale localeFromCode(const QString& code);

// 系統語言。只有明確是英文才回 en，其餘落回 zh_TW。
Locale systemLocale();

// .qm 所在目錄。安裝後在執行檔旁的 i18n/，開發時在建置輸出目錄的 i18n/。
QString translationsDirectory();

// 覆蓋率：已翻譯訊息數 / 來源訊息總數。zh_TW 恆為 1.0（它就是來源）。
// 讀取 .ts 檔案（設計期資料，非執行期常見路徑，但覆蓋率本來就是開發/驗收
// 才需要的數字，不必嵌進安裝包）。找不到檔案時回傳 0.0 並不視為錯誤——
// 呼叫端（本次任務報告）會明確寫出「檔案不存在」。
double translationCoverage(Locale locale, const QString& tsFilePath);

class LocaleManager : public QObject {
    Q_OBJECT

public:
    explicit LocaleManager(QCoreApplication* app, QObject* parent = nullptr);
    ~LocaleManager() override;

    [[nodiscard]] Locale current() const { return current_; }

    // 從 qmDirectory 載入對應 .qm 並安裝；zh_TW 則移除既有翻譯器（用原文）。
    // 回傳 false 代表找不到 .qm 檔（en 在覆蓋率不足 100% 時仍應該成功——
    // 找得到檔案就算成功，未翻譯的訊息本來就該退回原文，那不是錯誤）。
    bool switchTo(Locale locale, const QString& qmDirectory);

signals:
    void localeChanged(Locale locale);

private:
    QCoreApplication* app_;
    QTranslator* translator_{nullptr};
    Locale current_{Locale::zh_TW};
};

}  // namespace alioth::app
