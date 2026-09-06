#include "ui/ribbon/action_registry.h"

#include <QAction>

namespace alioth::ui::ribbon {

ActionRegistry::ActionRegistry(QObject* parent) : QObject(parent) {}

void ActionRegistry::detach(const QString& id) {
    const auto it = actions_.constFind(id);
    if (it == actions_.constEnd()) return;
    if (QAction* existing = it.value(); existing != nullptr) {
        disconnect(existing, nullptr, this, nullptr);
    }
}

bool ActionRegistry::registerAction(const QString& id, QAction* action) {
    if (id.isEmpty() || action == nullptr) return false;

    const bool replaced = actions_.contains(id);
    detach(id);
    actions_.insert(id, action);

    connect(action, &QAction::triggered, this, [this, id] { emit actionTriggered(id); });
    // QAction 的擁有者通常比 Registry 短命（例如隨文件關閉而消失），
    // 沒有這條清理，之後的 action(id) 會回傳懸空指標。
    connect(action, &QObject::destroyed, this, [this, id] {
        if (actions_.remove(id)) emit actionUnregistered(id);
    });

    emit actionRegistered(id);
    return replaced;
}

void ActionRegistry::unregisterAction(const QString& id) {
    detach(id);
    if (actions_.remove(id)) emit actionUnregistered(id);
}

QAction* ActionRegistry::action(const QString& id) const {
    const auto it = actions_.constFind(id);
    return it == actions_.constEnd() ? nullptr : it.value().data();
}

bool ActionRegistry::contains(const QString& id) const { return action(id) != nullptr; }

QStringList ActionRegistry::ids() const {
    QStringList result;
    result.reserve(actions_.size());
    for (auto it = actions_.constBegin(); it != actions_.constEnd(); ++it) {
        if (!it.value().isNull()) result << it.key();
    }
    result.sort();
    return result;
}

int ActionRegistry::count() const { return static_cast<int>(ids().size()); }

QStringList ActionRegistry::missingIds(const QStringList& required) const {
    QStringList missing;
    for (const QString& id : required) {
        if (!contains(id) && !missing.contains(id)) missing << id;
    }
    return missing;
}

}  // namespace alioth::ui::ribbon
