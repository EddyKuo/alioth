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
#include <QListWidget>
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

    // 縮圖清單的實況：有幾個項目、其中幾個真的拿到圖。
    // 「有 100 個項目但只有 1 個有圖」與「只有 1 個項目」在畫面上很像，
    // 但成因完全不同，光看截圖分不出來。
    if (const auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"))) {
        int withIcon = 0;
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->icon().isNull()) ++withIcon;
        }
        std::printf("thumbnails items=%d withIcon=%d\n", list->count(), withIcon);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Alioth"));
    QApplication::setOrganizationName(QStringLiteral("Alioth"));

    const QStringList args = QApplication::arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "用法: alioth_uishot <輸出.png> [寬 高] [文件.pdf]\n");
        return 2;
    }
    const QString output = args.at(1);

    // 視窗尺寸可指定。窄視窗的版面問題（工具列被裁掉、面板互相擠壓）只有
    // 在那個尺寸下才看得見，而預設的 1600×1000 剛好什麼問題都藏得住。
    int width = 1600;
    int height = 1000;
    if (args.size() >= 4) {
        bool okWidth = false;
        bool okHeight = false;
        const int w = args.at(2).toInt(&okWidth);
        const int h = args.at(3).toInt(&okHeight);
        if (!okWidth || !okHeight || w <= 0 || h <= 0) {
            std::fprintf(stderr, "寬與高必須是正整數\n");
            return 2;
        }
        width = w;
        height = h;
    }

    // 可選：開一份文件再截圖。面板的問題（縮圖只剩一頁、書籤是空的）
    // 在沒有文件的空視窗上完全看不到——那正是最容易漏掉的一類。
    const QString document = args.size() >= 5 ? args.at(4) : QString();

    alioth::ui::MainWindow window;
    window.resize(width, height);
    window.show();

    // 讓版面跑完一輪事件迴圈再抓圖：QWidget::grab 在第一次 show 之後、
    // 版面尚未生效之前抓到的是還沒排好的幾何。
    int exitCode = 0;
    if (!document.isEmpty()) window.openPath(document);

    // 開檔是非同步的（引擎執行緒），縮圖又比開檔晚一步。固定等一段時間
    // 而不是等訊號：這支工具的用途是「看畫面長什麼樣」，而畫面包含
    // 「載入到一半」那個狀態——等訊號會把它藏起來。
    const int delayMs = document.isEmpty() ? 0 : 3000;
    QTimer::singleShot(delayMs, &app, [&] {
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
