#pragma once

// 連結面板（PRD-UI-003，連結目標見 PRD-NAV-006）。
//
// 連結是 PDF 裡唯一「按下去會離開這份文件」的東西，而它在畫面上通常沒有任何
// 視覺標記——審閱者不會知道哪裡有連結、更不會知道連結指向哪裡。面板的價值
// 就在把那份隱藏的清單攤開來看。
//
// 兩個刻意的決定：
//
//   1. **外部網址完整顯示，而且不在這裡開啟。** 面板只負責把目的地攤在使用者
//      眼前；真的要開，走 MainWindow::followLink 那條確認流程（PRD §8.2：
//      開啟前確認、只放行 http/https）。面板自己呼叫 QDesktopServices 等於
//      多開一個繞過確認的側門。
//
//   2. **只列目前這一頁。** 連結是逐頁向引擎要的（DocumentController::requestLinks），
//      開檔就掃 500 頁會把渲染佇列塞滿，違反 PRD §8.1 的首頁預算。標題會寫明
//      現在列的是第幾頁，否則使用者會以為文件只有這幾個連結。

#include <QWidget>

#include "domain/document.h"

class QLabel;
class QTreeWidget;

namespace alioth::app {
class DocumentController;
}

namespace alioth::ui {

class LinksPanel : public QWidget {
    Q_OBJECT

public:
    explicit LinksPanel(alioth::app::DocumentController* controller, QWidget* parent = nullptr);

    // 切換到某一頁。pageIndex < 0 代表沒有開啟的文件。
    void setPage(int pageIndex);

    // 引擎把該頁的連結送回來時呼叫。頁碼不是目前這頁就忽略——
    // 快速翻頁時晚到的回覆會覆蓋掉新頁的內容。
    void linksArrived(int pageIndex);

signals:
    void linkActivated(const alioth::domain::LinkTarget& target);

private:
    void rebuild();

    alioth::app::DocumentController* controller_{nullptr};
    QLabel* status_{nullptr};
    QTreeWidget* tree_{nullptr};
    int pageIndex_{-1};
};

}  // namespace alioth::ui
