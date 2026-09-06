#pragma once

// Ribbon 內的單顆按鈕。
//
// 基底刻意選 QToolButton 而不是自繪：焦點框、按下狀態、無障礙名稱、圖示的
// devicePixelRatio 處理都由 Qt 樣式負責，自繪等於把這四件事全部重做一遍，
// 而其中任何一件做錯在高 DPI 或深色主題下都會立刻被看出來。

#include <QString>
#include <QToolButton>

#include "ui/ribbon/ribbon_model.h"

namespace alioth::ui::ribbon {

class RibbonButton : public QToolButton {
    Q_OBJECT

public:
    RibbonButton(const Item& item, QWidget* parent = nullptr);

    [[nodiscard]] const QString& actionId() const noexcept { return actionId_; }
    [[nodiscard]] ItemSize itemSize() const noexcept { return size_; }

    // 尚未注入 QAction 的項目會以停用的佔位按鈕呈現：設定檔可能是舊版留下的，
    // 靜靜地少一顆按鈕會讓使用者以為自己的自訂分頁掉了。
    [[nodiscard]] bool isPlaceholder() const noexcept { return placeholder_; }
    void setPlaceholder(bool placeholder);

    // 非 virtual 的遮蔽，不是覆寫：QToolButton::setIcon 不是虛擬函式。
    // 呼叫端一律持有 RibbonButton* 型別，因此遮蔽足夠；透過基底指標設圖示
    // 的路徑不存在。
    void setIcon(const QIcon& icon);

    // 觸控模式（PRD-UI-013）。開啟時每顆按鈕的命中區域不得小於 44 CSS px 等效。
    //
    // 不預設開啟：滑鼠使用者按 44px 的按鈕沒有任何好處，而整條 Ribbon 會因此
    // 明顯變高，把文件擠掉一截。手指與滑鼠的命中精度差一個量級，兩者用同一個
    // 尺寸必然有一邊吃虧，所以做成開關而不是折衷值。
    void setTouchMode(bool enabled);
    [[nodiscard]] bool touchMode() const noexcept { return touchMode_; }

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;

private:
    void applyMetrics();

    QString actionId_;
    ItemSize size_{ItemSize::Large};
    bool placeholder_{false};
    bool touchMode_{false};
};

}  // namespace alioth::ui::ribbon
