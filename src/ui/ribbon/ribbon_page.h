#pragma once

// Ribbon 的單一分頁：由左至右排列的群組。

#include <QList>
#include <QString>
#include <QWidget>

#include "ui/ribbon/icon_resolver.h"
#include "ui/ribbon/ribbon_model.h"

namespace alioth::ui::ribbon {

class ActionRegistry;
class RibbonButton;
class RibbonGroupWidget;

class RibbonPageWidget : public QWidget {
    Q_OBJECT

public:
    RibbonPageWidget(const Page& page, ActionRegistry* registry, IconResolver iconResolver,
                     QWidget* parent = nullptr);

    [[nodiscard]] const QString& pageId() const noexcept { return id_; }
    [[nodiscard]] const QList<RibbonGroupWidget*>& groups() const noexcept { return groups_; }
    [[nodiscard]] int groupCount() const { return static_cast<int>(groups_.size()); }
    [[nodiscard]] RibbonGroupWidget* group(int index) const;
    [[nodiscard]] RibbonGroupWidget* groupById(const QString& groupId) const;

    // 全分頁的按鈕總數（不含分隔線）。
    [[nodiscard]] int itemCount() const;
    [[nodiscard]] QList<RibbonButton*> buttons() const;
    [[nodiscard]] RibbonButton* buttonForActionId(const QString& actionId) const;

private:
    QString id_;
    QList<RibbonGroupWidget*> groups_;
};

}  // namespace alioth::ui::ribbon
