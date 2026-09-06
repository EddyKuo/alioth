#include "app/uisystem/tab_title_model.h"

#include <QDir>
#include <QFileInfo>

namespace alioth::app {

namespace {
QString baseNameOf(const TabTitleState& state) {
    if (!state.customLabel.isEmpty()) return state.customLabel;
    if (!state.filePath.isEmpty()) return QFileInfo(state.filePath).completeBaseName();
    return QStringLiteral("未命名文件");
}
}  // namespace

QString TabTitleModel::displayTitle(const TabTitleState& state) {
    QString title = baseNameOf(state);
    if (state.dirty) title += QStringLiteral(" *");
    return title;
}

QString TabTitleModel::tooltipText(const TabTitleState& state) {
    if (state.filePath.isEmpty()) return QStringLiteral("未命名文件（尚未儲存）");
    return QDir::toNativeSeparators(state.filePath);
}

std::vector<TabTitleModel::DisambiguatedTitle> TabTitleModel::disambiguate(
    const std::vector<TabTitleState>& states) {
    std::vector<DisambiguatedTitle> results(states.size());

    // 依「顯示標題」分組。自訂標籤的頁籤不參與消歧——使用者已經明確給了名字，
    // 就算撞名也是使用者的選擇。比對一律用原始標題（originalTitles），不能用
    // results[].title——那個欄位會在迴圈中被逐一改寫成消歧後的文字，若拿它來比對，
    // 後面處理到的項目會比對到「已經被改過的」標題而永遠比不中，造成漏消歧。
    std::vector<QString> originalTitles(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        originalTitles[i] = displayTitle(states[i]);
        results[i].title = originalTitles[i];
    }
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (!states[i].customLabel.isEmpty() || states[i].filePath.isEmpty()) continue;
        int duplicateCount = 0;
        for (std::size_t j = 0; j < states.size(); ++j) {
            if (i == j) continue;
            if (!states[j].customLabel.isEmpty() || states[j].filePath.isEmpty()) continue;
            if (originalTitles[i] == originalTitles[j]) ++duplicateCount;
        }
        if (duplicateCount > 0) {
            const QString parentDir = QFileInfo(states[i].filePath).dir().dirName();
            results[i].title = QStringLiteral("%1 — %2").arg(displayTitle(states[i]), parentDir);
            results[i].disambiguated = true;
        }
    }
    return results;
}

TabTitleModel::RenamePlan TabTitleModel::planRename(const QString& currentPath,
                                                     const QString& newBaseName) {
    RenamePlan plan;
    if (currentPath.isEmpty()) {
        plan.error = QStringLiteral("尚未儲存的文件不能重新命名，請先另存新檔");
        return plan;
    }
    if (newBaseName.trimmed().isEmpty()) {
        plan.error = QStringLiteral("檔名不可為空");
        return plan;
    }
    if (newBaseName.contains(QLatin1Char('/')) || newBaseName.contains(QLatin1Char('\\'))) {
        plan.error = QStringLiteral("檔名不可包含路徑分隔字元");
        return plan;
    }
    static const QString kForbidden = QStringLiteral(":*?\"<>|");
    for (const QChar ch : newBaseName) {
        if (kForbidden.contains(ch)) {
            plan.error = QStringLiteral("檔名含不合法字元：%1").arg(ch);
            return plan;
        }
    }

    const QFileInfo currentInfo(currentPath);
    QString base = newBaseName;
    QString extension = currentInfo.suffix();
    // 使用者若在新名稱裡自己帶了副檔名就尊重它；否則沿用原副檔名。
    const QFileInfo newInfo(newBaseName);
    if (!newInfo.suffix().isEmpty()) {
        base = newInfo.completeBaseName();
        extension = newInfo.suffix();
    }

    const QString fileName = extension.isEmpty() ? base : QStringLiteral("%1.%2").arg(base, extension);
    plan.newPath = currentInfo.dir().filePath(fileName);
    if (plan.newPath == currentPath) {
        plan.error = QStringLiteral("新檔名與原檔名相同");
        return plan;
    }
    plan.ok = true;
    return plan;
}

}  // namespace alioth::app
