#pragma once

// 自研 Ribbon 本體（WBS 3.2 / PRD-UI-002）。ADR-001 已定案不引入 SARibbon 或商業元件。
//
// 組成：快速存取工具列（QAT）+ 分頁列 + 分頁內容（群組與按鈕）+ 可收合 + KeyTips。
// 內容全部由 Layout 這份資料描述，換一份資料就是換一套 Ribbon——這是 PRD-UI-011
// 「使用者自組分頁」的實作基礎，也讓整個元件可以在沒有主視窗的情況下測試。

#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "ui/ribbon/icon_resolver.h"
#include "ui/ribbon/ribbon_model.h"

class QHBoxLayout;
class QStackedWidget;
class QTabBar;

namespace alioth::ui::ribbon {

class ActionRegistry;
class KeyTipController;
class RibbonButton;
class RibbonPageWidget;

class RibbonBar : public QWidget {
    Q_OBJECT

public:
    explicit RibbonBar(ActionRegistry* registry, QWidget* parent = nullptr);

    // 要在 setLayoutModel() 之前設定才會套用到既有按鈕；預設為 resolveIconByName。
    void setIconResolver(IconResolver resolver);

    void setLayoutModel(const Layout& layout);
    [[nodiscard]] const Layout& layoutModel() const noexcept { return layout_; }

    [[nodiscard]] int pageCount() const;
    [[nodiscard]] RibbonPageWidget* page(int index) const;
    [[nodiscard]] RibbonPageWidget* pageById(const QString& pageId) const;
    [[nodiscard]] int currentPageIndex() const;
    [[nodiscard]] QString currentPageId() const;
    void setCurrentPageIndex(int index);
    bool setCurrentPageId(const QString& pageId);

    [[nodiscard]] bool isCollapsed() const noexcept { return collapsed_; }
    void setCollapsed(bool collapsed);

    // 觸控模式（PRD-UI-013）：所有按鈕的命中區域放大到 44 CSS px 等效。
    // 設定後建立的按鈕（切換配置、rebind）也會沿用，否則自訂分頁的按鈕
    // 會停在滑鼠尺寸，而使用者看不出為什麼只有那幾顆按不準。
    void setTouchMode(bool enabled);
    [[nodiscard]] bool touchMode() const noexcept { return touchMode_; }

    [[nodiscard]] QTabBar* tabBar() const noexcept { return tabBar_; }
    [[nodiscard]] const QList<RibbonButton*>& quickAccessButtons() const noexcept {
        return quickAccessButtons_;
    }
    [[nodiscard]] RibbonButton* buttonForActionId(const QString& actionId) const;
    // 配置裡有、registry 裡沒有的 id。整合端啟動時可據此發現設定檔與程式版本脫節。
    [[nodiscard]] QStringList missingActionIds() const;

    // KeyTips。第 0 層提示分頁與快速存取列，第 1 層提示目前分頁內的項目。
    void showKeyTips();
    void hideKeyTips();
    [[nodiscard]] bool keyTipsVisible() const;
    [[nodiscard]] int keyTipLevel() const noexcept { return keyTipLevel_; }
    [[nodiscard]] QStringList visibleKeyTips() const;
    // 直接送一個 KeyTip 字元；鍵盤路徑與這裡走同一份邏輯。
    bool handleKeyTip(QChar character);

signals:
    void currentPageChanged(int index);
    void collapsedChanged(bool collapsed);
    // 任一已註冊動作被觸發時轉發。Ribbon 不區分是按鈕、KeyTip 還是外部快捷鍵觸發的，
    // 整合端只需要接這一條線就能知道「使用者要做什麼」。
    void actionTriggered(const QString& actionId);
    void layoutModelChanged();

private:
    void buildChrome();
    void rebuild();
    void rebuildQuickAccess();
    void updateKeyTipTargets();
    void setKeyTipLevel(int level);

    ActionRegistry* registry_{nullptr};
    IconResolver iconResolver_;
    Layout layout_;
    // 補齊 KeyTip 之後的副本。原始 layout_ 保持使用者給的樣子，
    // 否則存回設定檔時會多出一堆使用者沒寫過的 keyTip 欄位。
    Layout effective_;

    QTabBar* tabBar_{nullptr};
    QStackedWidget* stack_{nullptr};
    QWidget* quickAccessBar_{nullptr};
    QHBoxLayout* quickAccessLayout_{nullptr};
    QList<RibbonButton*> quickAccessButtons_;
    bool touchMode_{false};
    QList<RibbonPageWidget*> pages_;
    KeyTipController* keyTips_{nullptr};

    bool collapsed_{false};
    // 單擊與雙擊的去重時間窗，見 buildChrome() 中的說明。
    QElapsedTimer sinceDoubleClick_;
    QElapsedTimer sinceClickExpand_;
    int keyTipLevel_{0};
};

}  // namespace alioth::ui::ribbon
