#pragma once

// Ribbon 的群組：一組相關按鈕加下方標題，右側以一條細線與鄰組分隔。

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "ui/ribbon/icon_resolver.h"
#include "ui/ribbon/ribbon_model.h"

class QAction;
class QLabel;
class QHBoxLayout;

namespace alioth::ui::ribbon {

class ActionRegistry;
class RibbonButton;

class RibbonGroupWidget : public QWidget {
    Q_OBJECT

public:
    RibbonGroupWidget(const Group& group, ActionRegistry* registry, IconResolver iconResolver,
                      QWidget* parent = nullptr);

    [[nodiscard]] const QString& groupId() const noexcept { return id_; }
    [[nodiscard]] QString title() const;
    [[nodiscard]] const QList<RibbonButton*>& buttons() const noexcept { return buttons_; }
    [[nodiscard]] int itemCount() const { return static_cast<int>(buttons_.size()); }
    [[nodiscard]] RibbonButton* buttonForActionId(const QString& actionId) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void build(const Group& group);
    RibbonButton* makeButton(const Item& item);
    // 重新以目前 registry 的內容設定按鈕。動作晚注入或被撤下時都走這裡。
    void rebind(RibbonButton* button);
    void applyTitlePalette();

    ActionRegistry* registry_{nullptr};
    IconResolver iconResolver_;
    QString id_;
    QLabel* titleLabel_{nullptr};
    QHBoxLayout* contentLayout_{nullptr};
    QList<RibbonButton*> buttons_;
    QHash<QString, Item> items_;
    QHash<QString, QPointer<QAction>> boundActions_;
};

}  // namespace alioth::ui::ribbon
