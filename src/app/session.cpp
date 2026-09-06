#include "app/session.h"

#include <QFileInfo>
#include <QSettings>

namespace alioth::app {
namespace {

constexpr const char* kGroup = "session";
constexpr const char* kDocuments = "documents";
constexpr const char* kActive = "activeIndex";
constexpr const char* kGeometry = "windowGeometry";
constexpr const char* kState = "windowState";

// 版面模式以整數存。存字串比較好讀，但一旦有人改了列舉順序，
// 整數會安靜地變成另一種版面，而字串只會讀不到然後退回預設。
constexpr const char* kMode = "layoutMode";

domain::LayoutMode modeFromString(const QString& text) {
    if (text == QStringLiteral("single")) return domain::LayoutMode::SinglePage;
    if (text == QStringLiteral("twoPage")) return domain::LayoutMode::TwoPage;
    if (text == QStringLiteral("twoPageContinuous")) return domain::LayoutMode::TwoPageContinuous;
    if (text == QStringLiteral("horizontal")) return domain::LayoutMode::Horizontal;
    return domain::LayoutMode::Continuous;
}

QString modeToString(domain::LayoutMode mode) {
    switch (mode) {
        case domain::LayoutMode::SinglePage:        return QStringLiteral("single");
        case domain::LayoutMode::TwoPage:           return QStringLiteral("twoPage");
        case domain::LayoutMode::TwoPageContinuous: return QStringLiteral("twoPageContinuous");
        case domain::LayoutMode::Horizontal:        return QStringLiteral("horizontal");
        case domain::LayoutMode::Continuous:        break;
    }
    return QStringLiteral("continuous");
}

}  // namespace

Session loadSession() {
    Session session;
    QSettings settings;
    settings.beginGroup(kGroup);

    session.activeIndex = settings.value(kActive, 0).toInt();
    session.windowGeometry = settings.value(kGeometry).toByteArray();
    session.windowState = settings.value(kState).toByteArray();

    const int count = settings.beginReadArray(kDocuments);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);

        DocumentSession document;
        document.path = settings.value(QStringLiteral("path")).toString();
        // 檔案可能已經被刪除或搬走。還原到一個開不起來的檔案，
        // 使用者看到的是啟動時跳一個錯誤對話框——那比什麼都不還原更糟。
        if (document.path.isEmpty() || !QFileInfo::exists(document.path)) continue;

        document.pageIndex = settings.value(QStringLiteral("pageIndex"), 0).toInt();
        document.scale = settings.value(QStringLiteral("scale"), 1.0).toDouble();
        document.layoutMode = modeFromString(settings.value(kMode).toString());
        document.coverPageSeparate =
            settings.value(QStringLiteral("coverPageSeparate"), true).toBool();
        document.rightToLeft = settings.value(QStringLiteral("rightToLeft"), false).toBool();

        // 明顯不合理的值一律退回預設而不是照單全收：設定檔可能被手改壞，
        // 而一個 0 倍的縮放會讓畫面完全空白，使用者無從判斷發生了什麼事。
        if (!(document.scale > 0.0) || document.scale > 64.0) document.scale = 1.0;
        if (document.pageIndex < 0) document.pageIndex = 0;

        session.documents.push_back(std::move(document));
    }
    settings.endArray();
    settings.endGroup();

    if (session.activeIndex < 0 ||
        session.activeIndex >= static_cast<std::int32_t>(session.documents.size())) {
        session.activeIndex = 0;
    }
    return session;
}

void saveSession(const Session& session) {
    QSettings settings;
    settings.beginGroup(kGroup);

    settings.setValue(kActive, session.activeIndex);
    settings.setValue(kGeometry, session.windowGeometry);
    settings.setValue(kState, session.windowState);

    // 先清掉舊陣列：QSettings 的 beginWriteArray 不會移除多出來的舊項目，
    // 文件數變少時會留下上一次的殘留。
    settings.remove(kDocuments);
    settings.beginWriteArray(kDocuments, static_cast<int>(session.documents.size()));
    for (int i = 0; i < static_cast<int>(session.documents.size()); ++i) {
        const DocumentSession& document = session.documents[static_cast<std::size_t>(i)];
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("path"), document.path);
        settings.setValue(QStringLiteral("pageIndex"), document.pageIndex);
        settings.setValue(QStringLiteral("scale"), document.scale);
        settings.setValue(kMode, modeToString(document.layoutMode));
        settings.setValue(QStringLiteral("coverPageSeparate"), document.coverPageSeparate);
        settings.setValue(QStringLiteral("rightToLeft"), document.rightToLeft);
    }
    settings.endArray();
    settings.endGroup();
}

void clearSession() {
    QSettings settings;
    settings.remove(kGroup);
}

}  // namespace alioth::app
