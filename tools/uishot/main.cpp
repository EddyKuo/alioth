// 主視窗截圖與版面傾印工具。
//
// 存在理由：版面問題是視覺問題。只讀程式碼判斷「按鈕排得對不對」會漏掉
// 空圖示佔位、Ribbon 被工具列擠到視窗中段、面板搶走檢視區這幾類缺陷——
// 它們在程式碼上都看起來完全正常。
//
// 除了圖，也把停靠面板與工具列的可見狀態與幾何印出來：從截圖上分不出
// 「面板還在」與「面板收起來了，但別的東西佔住那塊位置」。
//
// 注意：本工具會讀取使用者存下來的版面狀態。要看「首次啟動的預設版面」，
// 必須先清掉 QSettings 的內容（Windows 上在 HKCU\Software\Alioth），
// 否則看到的是上一次執行留下的版面——這個坑已經誤導過一次。
//
// 走 offscreen 平台外掛，因此不需要桌面工作階段。
//
// 用法：alioth_uishot <輸出.png>

#include <QApplication>
#include <QDockWidget>
#include <QMenuBar>
#include <QPixmap>
#include <QTimer>
#include <QToolBar>

#include <cstdio>

#include "ui/main_window.h"

namespace {

void dumpLayout(const alioth::ui::MainWindow& window) {
    for (const QDockWidget* dock : window.findChildren<QDockWidget*>()) {
        std::printf("dock    %-28s visible=%d\n", qPrintable(dock->objectName()),
                    dock->isVisible() ? 1 : 0);
    }
    for (const QToolBar* bar : window.findChildren<QToolBar*>()) {
        const QRect rect = bar->geometry();
        std::printf("toolbar %-28s visible=%d geometry=%d,%d %dx%d\n",
                    qPrintable(bar->objectName()), bar->isVisible() ? 1 : 0, rect.x(), rect.y(),
                    rect.width(), rect.height());
    }
    std::printf("menubar visible=%d\n", window.menuBar()->isVisible() ? 1 : 0);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Alioth"));
    QApplication::setOrganizationName(QStringLiteral("Alioth"));

    const QStringList args = QApplication::arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "用法: alioth_uishot <輸出.png>\n");
        return 2;
    }
    const QString output = args.at(1);

    alioth::ui::MainWindow window;
    window.resize(1600, 1000);
    window.show();

    // 讓版面跑完一輪事件迴圈再抓圖：QWidget::grab 在第一次 show 之後、
    // 版面尚未生效之前抓到的是還沒排好的幾何。
    int exitCode = 0;
    QTimer::singleShot(0, &app, [&] {
        dumpLayout(window);
        const QPixmap shot = window.grab();
        if (!shot.save(output)) {
            std::fprintf(stderr, "寫入失敗: %s\n", qPrintable(output));
            exitCode = 1;
        }
        QApplication::quit();
    });

    QApplication::exec();
    return exitCode;
}
