#pragma once

// 簽章面板（PRD-SIG-002）。
//
// 這個面板的正確性判準只有一條：**不受信任的東西不可以看起來像受信任的**。
// 因此三態燈號直接對應引擎的判定，中間不做任何「使用者友善」的簡化——
// 把黃燈說成「大致沒問題」是這類介面最危險的設計。
//
// 顏色不單獨承載意義：色盲使用者看不出綠與紅的差別，所以每一列都同時有文字標籤。

#include <QWidget>

#include <vector>

class QLabel;
class QTreeWidget;

namespace alioth::app {
class SignatureController;
}

namespace alioth::ui {

class SignaturePanel : public QWidget {
    Q_OBJECT

public:
    SignaturePanel(app::SignatureController* controller, QWidget* parent = nullptr);

    // 文件換了之後由呼叫端觸發驗證。面板不自己決定何時驗——
    // 驗證要算整份檔案的雜湊，在使用者只是翻頁時做那件事是浪費。
    void refresh();

private:
    void rebuild();

    app::SignatureController* controller_{nullptr};
    QTreeWidget* tree_{nullptr};
    QLabel* summary_{nullptr};
};

}  // namespace alioth::ui
