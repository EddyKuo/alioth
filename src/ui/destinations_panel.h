#pragma once

// 命名目標面板（PRD-NAV-007）。
//
// 命名目標是 PDF 裡「具名的位置」，交叉參照、目錄、外部連結都靠它。
// 面板的價值在於讓審閱者看見文件作者定義了哪些錨點——尤其是規範類文件，
// 條號往往就是命名目標。
//
// 解析不到目標頁的項目照樣列出但不可點：真實文件裡指向已刪除頁面的命名目標
// 很常見，靜默跳到第一頁會讓使用者以為自己看錯了條號。

#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace alioth::app {
class NavigationService;
}

namespace alioth::ui {

class DestinationsPanel : public QWidget {
    Q_OBJECT

public:
    explicit DestinationsPanel(alioth::app::NavigationService* service,
                               QWidget* parent = nullptr);

    // 換文件時呼叫。空路徑代表關閉文件。
    void setDocument(const QString& path);

signals:
    // 第二個參數為 true 時代表目標沒指定倍率，呼叫端應沿用目前倍率。
    void destinationActivated(int pageIndex, bool inheritZoom);

private:
    void rebuild();

    alioth::app::NavigationService* service_{nullptr};
    QLineEdit* search_{nullptr};
    QLabel* status_{nullptr};
    QTreeWidget* tree_{nullptr};
};

}  // namespace alioth::ui
