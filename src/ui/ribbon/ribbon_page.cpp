#include "ui/ribbon/ribbon_page.h"

#include <QFontMetrics>
#include <QHBoxLayout>

#include <algorithm>

#include "ui/ribbon/ribbon_button.h"
#include "ui/ribbon/ribbon_group.h"

namespace alioth::ui::ribbon {

RibbonPageWidget::RibbonPageWidget(const Page& page, ActionRegistry* registry,
                                   IconResolver iconResolver, QWidget* parent)
    : QWidget(parent), id_(page.id) {
    setObjectName(page.id);

    const int unit = std::max(2, QFontMetrics(font()).height() / 4);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(unit, unit / 2, unit, unit / 2);
    layout->setSpacing(unit);

    for (const Group& group : page.groups) {
        auto* widget = new RibbonGroupWidget(group, registry, iconResolver, this);
        groups_.append(widget);
        layout->addWidget(widget);
    }
    // 群組靠左，右側留白；缺了這個 stretch，群組會被平均拉寬成一片空按鈕。
    layout->addStretch(1);
}

RibbonGroupWidget* RibbonPageWidget::group(int index) const {
    if (index < 0 || index >= groups_.size()) return nullptr;
    return groups_.at(index);
}

RibbonGroupWidget* RibbonPageWidget::groupById(const QString& groupId) const {
    for (RibbonGroupWidget* group : groups_) {
        if (group->groupId() == groupId) return group;
    }
    return nullptr;
}

int RibbonPageWidget::itemCount() const {
    int total = 0;
    for (const RibbonGroupWidget* group : groups_) total += group->itemCount();
    return total;
}

QList<RibbonButton*> RibbonPageWidget::buttons() const {
    QList<RibbonButton*> result;
    for (const RibbonGroupWidget* group : groups_) result.append(group->buttons());
    return result;
}

RibbonButton* RibbonPageWidget::buttonForActionId(const QString& actionId) const {
    for (const RibbonGroupWidget* group : groups_) {
        if (RibbonButton* button = group->buttonForActionId(actionId); button != nullptr) {
            return button;
        }
    }
    return nullptr;
}

}  // namespace alioth::ui::ribbon
