// Alioth — 跨平台專業 PDF 審閱工作站。
//
// 目標平台目前為 Windows x64；macOS 與 Linux 的移植成本靠「作業系統差異只在
// platform 層」這條規則壓住，見 CLAUDE.md。

#include <QApplication>
#include <QCommandLineParser>

#include "app/settings.h"
#include "app/uisystem/locale_manager.h"
#include "ui/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Alioth"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("Alioth"));

    // 語言必須在建出任何 widget 之前裝好（PRD-UI-005）。Qt 的 tr() 是在字串
    // 被求值的當下查表，而選單與按鈕的文字都在建構式裡就決定了——晚一步安裝
    // 翻譯器，整個介面就會停在原文，而且看起來像翻譯檔壞掉。
    alioth::app::Settings settings;
    alioth::app::LocaleManager locales(&app);
    locales.switchTo(settings.locale(), alioth::app::translationsDirectory());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Alioth PDF 審閱工作站"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("要開啟的 PDF 檔案"));
    parser.process(app);

    alioth::ui::MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        window.openPath(args.first());
    }

    return QApplication::exec();
}
