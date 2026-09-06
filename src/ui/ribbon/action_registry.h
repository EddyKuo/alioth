#pragma once

// actionId → QAction 的掛載點。
//
// Ribbon 只認得字串 id，具體的 QAction 由整合端（main_window）注入。這道間接層是
// 自訂 Ribbon 能成立的關鍵：使用者在設定檔裡寫 "view.zoomIn"，Ribbon 不需要知道
// 放大是什麼，也不需要為了組出按鈕而反向依賴應用層。副作用是 Ribbon 可以在測試裡
// 用假的 QAction 完整驗證，不必拖進整個文件控制器。

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QAction;

namespace alioth::ui::ribbon {

class ActionRegistry : public QObject {
    Q_OBJECT

public:
    explicit ActionRegistry(QObject* parent = nullptr);

    // 同一個 id 重複註冊時後者取代前者，回傳是否覆寫了既有項目。
    // 允許覆寫是為了讓整合端能分階段注入（例如文件開啟後才有的動作）。
    bool registerAction(const QString& id, QAction* action);
    void unregisterAction(const QString& id);

    [[nodiscard]] QAction* action(const QString& id) const;
    [[nodiscard]] bool contains(const QString& id) const;
    [[nodiscard]] QStringList ids() const;
    [[nodiscard]] int count() const;

    // 配置裡有、但沒被注入的 id。整合端可以據此在啟動時就發現設定檔與程式版本脫節，
    // 而不是等使用者點到那顆沒反應的按鈕才發現。
    [[nodiscard]] QStringList missingIds(const QStringList& required) const;

signals:
    // 任一已註冊動作被觸發時發出。Ribbon 以此對外轉述，整合端不必逐顆按鈕接線。
    void actionTriggered(const QString& id);
    void actionRegistered(const QString& id);
    void actionUnregistered(const QString& id);

private:
    void detach(const QString& id);

    QHash<QString, QPointer<QAction>> actions_;
};

}  // namespace alioth::ui::ribbon
