#pragma once

// KeyTips：按 Alt 顯示按鍵提示，再按字母逐層深入（PRD-UI-002 的鍵盤操作部分，
// 同時是 PRD-A11Y-005「全鍵盤」的一環）。
//
// 這個類別只管三件事：什麼時候該顯示提示、提示標籤畫在哪、按下的字元對應到誰。
// 層級語意（第一層是分頁、第二層是分頁內的項目）屬於 Ribbon，不在這裡，
// 否則控制器會被迫認識分頁與群組的結構。

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>

#include <functional>

class QLabel;
class QWidget;

namespace alioth::ui::ribbon {

class KeyTipController : public QObject {
    Q_OBJECT

public:
    struct Target {
        QString keyTip;
        // 以 host 座標表示。用矩形而不是 widget 指標，是因為分頁標籤是 QTabBar
        // 內部的一塊區域，本身不是獨立的 widget。
        QRect anchorRect;
        std::function<void()> activate;
    };

    explicit KeyTipController(QWidget* host, QObject* parent = nullptr);
    ~KeyTipController() override;

    void setTargets(QList<Target> targets);
    [[nodiscard]] const QList<Target>& targets() const noexcept { return targets_; }

    void showTips();
    void hideTips();
    [[nodiscard]] bool isShowing() const noexcept { return showing_; }
    [[nodiscard]] QStringList visibleKeyTips() const;

    // 送一個字元進來。命中就執行該 Target 的 activate 並回傳 true。
    // 測試與實際的鍵盤路徑都走這個函式，避免兩條路徑行為不一致。
    bool handleKey(QChar character);

signals:
    // 使用者按下 Alt。要顯示哪一層由 Ribbon 決定，所以這裡只是請求。
    void activationRequested();
    // 使用者按下 Esc：回上一層或整個關閉，同樣由 Ribbon 決定。
    void backRequested();
    void keyTipActivated(const QString& keyTip);
    void dismissed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void clearLabels();
    void placeLabels();
    [[nodiscard]] bool belongsToHost(const QObject* watched) const;

    QPointer<QWidget> host_;
    QList<Target> targets_;
    QList<QLabel*> labels_;
    bool showing_{false};
};

}  // namespace alioth::ui::ribbon
