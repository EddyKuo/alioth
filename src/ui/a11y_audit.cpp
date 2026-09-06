#include "ui/a11y_audit.h"

#include <QAccessible>
#include <QAccessibleInterface>
#include <QAction>
#include <QMenu>
#include <QPalette>
#include <QToolButton>
#include <QRegularExpression>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace alioth::ui::a11y {
namespace {

// 需要鍵盤可達的元件類別。用字串而不是 dynamic_cast，是為了不必在呈現層的
// 稽核模組裡 include 二十個 Qt widget 標頭——那會讓這個檔案的編譯時間
// 比它檢查的東西還長，也會在 Qt 升版新增類別時要跟著改。
const char* const kInteractiveClasses[] = {
    "QAbstractButton", "QLineEdit",      "QAbstractItemView", "QComboBox",
    "QAbstractSlider", "QAbstractSpinBox", "QTabBar",         "QPlainTextEdit",
    "QTextEdit",       "QMenuBar",       "QGroupBox",
};

// Qt 內部元件，不是使用者介面的一部分：捲動區的 viewport 與捲軸容器、
// 捲軸本身、分頁的角落元件等。它們刻意不接受焦點，因為焦點屬於外層容器
// （捲軸的操作方式是把焦點放在捲動區上按方向鍵，不是 Tab 到捲軸）。
//
// 判定要看整條祖先鏈而不只是直接父層：QScrollBar 的父層是
// qt_scrollarea_vcontainer，再上一層才是捲動區。只看一層會漏掉，
// 而漏掉的後果是每個清單都報三筆假缺失，很快就沒有人看這份報告了。
[[nodiscard]] bool hasQtInternalAncestor(const QWidget* widget) {
    for (const QWidget* node = widget; node != nullptr; node = node->parentWidget()) {
        if (node->objectName().startsWith(QLatin1String("qt_"))) return true;
    }
    return false;
}

// 複合元件的內部零件：QComboBox 的 QLineEdit、QSpinBox 的 QLineEdit、
// 捲動區的捲軸。焦點與可及性名稱都屬於外層那一個。
[[nodiscard]] bool isCompositeChild(const QWidget* widget) {
    if (widget->inherits("QScrollBar")) return true;
    // QLineEdit 的清除按鈕（setClearButtonEnabled）。它是 Qt 生出來的，
    // 而且功能上等同「全選後按 Delete」，鍵盤使用者本來就有路徑。
    if (QString::fromLatin1(widget->metaObject()->className()) ==
        QLatin1String("QLineEditIconButton")) {
        return true;
    }
    // 下拉選單的彈出容器與它的清單。這兩個是 QComboBox 私有實作生出來的，
    // 名稱與焦點都屬於外層的 QComboBox——鍵盤使用者是在 combo 上按方向鍵，
    // 從來不會 Tab 到那張清單。少了這一條，每個下拉選單都會報兩筆假缺失。
    const QString className = QString::fromLatin1(widget->metaObject()->className());
    if (className == QLatin1String("QComboBoxPrivateContainer") ||
        className == QLatin1String("QComboBoxListView")) {
        return true;
    }

    for (const QWidget* node = widget->parentWidget(); node != nullptr;
         node = node->parentWidget()) {
        if (QString::fromLatin1(node->metaObject()->className()) ==
            QLatin1String("QComboBoxPrivateContainer")) {
            return true;
        }
    }

    const QWidget* parent = widget->parentWidget();
    if (parent == nullptr) return false;
    return parent->inherits("QComboBox") || parent->inherits("QAbstractSpinBox") ||
           parent->inherits("QAbstractItemView") || parent->inherits("QAbstractScrollArea") ||
           parent->inherits("QLineEdit");
}

// 框架自動產生的元件：QToolBar 依 QAction 生出的 QToolButton、
// QMainWindow 為 tabify 的面板生出的 QMainWindowTabBar、停靠面板的
// 標題列按鈕。這些不是我們 new 出來的，設不了 objectName，
// 要求它們有 AutomationId 只會產生永遠修不掉的缺失。
[[nodiscard]] bool isFrameworkGenerated(const QWidget* widget) {
    const QString klass = QString::fromLatin1(widget->metaObject()->className());
    if (klass == QLatin1String("QMainWindowTabBar") ||
        klass == QLatin1String("QDockWidgetTitleButton") ||
        klass == QLatin1String("QToolBarExtension") ||
        klass == QLatin1String("QMenuBarExtension")) {
        return true;
    }
    const QWidget* parent = widget->parentWidget();
    return parent != nullptr && (parent->inherits("QToolBar") || parent->inherits("QMenuBar") ||
                                 parent->inherits("QStatusBar"));
}

// 工具列按鈕的鍵盤可達性判定（WCAG 2.1.1）。
//
// 條文要求的是「功能可用鍵盤達成」，不是「每個 widget 都能被 Tab 選到」。
// Qt 的 QToolButton 依設計是 NoFocus，這不是缺陷——只要同一個 QAction
// 也掛在選單上或有快捷鍵，鍵盤使用者就有路徑。反過來說，只出現在工具列
// 又沒有快捷鍵的動作，鍵盤使用者是真的做不到，那才是要抓的。
[[nodiscard]] bool actionIsReachable(const QAction* action) {
    if (action == nullptr) return false;
    if (!action->shortcut().isEmpty()) return true;
    if (action->menu() != nullptr) return true;
    // 同一個 QAction 也被加進某個 QMenu：使用者可以循選單列到達。
    const QList<QObject*> owners = action->associatedObjects();
    for (const QObject* owner : owners) {
        if (owner->inherits("QMenu") || owner->inherits("QMenuBar")) return true;
    }
    return false;
}

[[nodiscard]] bool toolButtonActionIsReachable(const QWidget* widget) {
    const auto* button = qobject_cast<const QToolButton*>(widget);
    if (button == nullptr) return false;
    // defaultAction() 在 QToolBar 生出來的按鈕上可能是空的（動作掛在
    // actions() 而非 default），因此兩邊都要看。只查 defaultAction 會讓
    // 整條工具列被誤判成鍵盤不可達。
    if (actionIsReachable(button->defaultAction())) return true;
    const QList<QAction*> actions = button->actions();
    for (const QAction* action : actions) {
        if (actionIsReachable(action)) return true;
    }
    return false;
}

[[nodiscard]] bool isInteractive(const QWidget* widget) {
    if (hasQtInternalAncestor(widget) || isCompositeChild(widget)) return false;
    for (const char* klass : kInteractiveClasses) {
        if (widget->inherits(klass)) {
            // 唯讀的文字元件是內容不是控制項，NoFocus 是合理的設計。
            // 不過它們仍然要有可及性名稱，所以只在鍵盤可達性上放行。
            return true;
        }
    }
    // 自訂元件若已經宣告自己要焦點，就納入名稱與角色檢查。
    return widget->focusPolicy() != Qt::NoFocus;
}

// QGroupBox 只有 checkable 時才是控制項；否則它是分組標題。
[[nodiscard]] bool requiresKeyboardReach(const QWidget* widget) {
    // 選單列的鍵盤入口是 Alt 與 F10，由視窗系統處理，不是 Tab。
    // 它的 focusPolicy 依設計就是 NoFocus，要求它可 Tab 是誤判。
    if (widget->inherits("QMenuBar")) return false;
    // 工具列按鈕依 Qt 設計是 NoFocus；只要對應動作另有選單或快捷鍵路徑就合格。
    if (widget->inherits("QToolButton") && toolButtonActionIsReachable(widget)) return false;
    if (widget->inherits("QGroupBox")) {
        return widget->property("checkable").toBool();
    }
    if (widget->inherits("QPlainTextEdit") || widget->inherits("QTextEdit")) {
        // 唯讀檢視器仍應可 Tab 進去捲動，但 Qt 預設就給了焦點；
        // 明確設成 NoFocus 是刻意的，不視為缺失。
        return widget->focusPolicy() != Qt::NoFocus;
    }
    return true;
}

[[nodiscard]] QString widgetPath(const QWidget* widget) {
    QStringList parts;
    for (const QWidget* node = widget; node != nullptr; node = node->parentWidget()) {
        const QString name = node->objectName();
        parts.prepend(name.isEmpty() ? QString::fromLatin1(node->metaObject()->className())
                                     : name);
        if (parts.size() > 12) break;  // 深層巢狀時只保留可辨識的尾段
    }
    return parts.join(QLatin1String(" / "));
}

[[nodiscard]] Finding makeFinding(FindingKind kind, const QWidget* widget, QString detail) {
    Finding finding;
    finding.kind = kind;
    finding.widgetClass = QString::fromLatin1(widget->metaObject()->className());
    finding.objectName = widget->objectName();
    finding.path = widgetPath(widget);
    finding.detail = std::move(detail);
    return finding;
}

// sRGB 分量線性化（WCAG 2.1 relative luminance 定義）。
[[nodiscard]] double linearize(double channel) {
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

void checkContrastPair(std::vector<Finding>& out, const QColor& fg, const QColor& bg,
                       const QString& context, const QString& pairName) {
    const double ratio = contrastRatio(fg, bg);
    if (ratio >= kContrastAaNormal) return;
    Finding finding;
    finding.kind = FindingKind::InsufficientContrast;
    finding.widgetClass = QStringLiteral("QPalette");
    finding.objectName = context;
    finding.path = context;
    finding.detail = QStringLiteral("%1：%2 對 %3 的對比為 %4:1，未達 WCAG AA 的 4.5:1")
                         .arg(pairName, fg.name(), bg.name(), QString::number(ratio, 'f', 2));
    out.push_back(finding);
}

}  // namespace

const char* describe(FindingKind kind) noexcept {
    switch (kind) {
        case FindingKind::NotKeyboardReachable:     return "鍵盤無法到達";
        case FindingKind::MissingAccessibleName:    return "缺少可及性名稱";
        case FindingKind::MissingAutomationId:      return "缺少 objectName";
        case FindingKind::UninformativeRole:        return "可及性角色過於泛用";
        case FindingKind::HardcodedStyleSheetColor: return "樣式表硬編顏色";
        case FindingKind::InsufficientContrast:     return "對比不足";
    }
    return "未知";
}

QString Finding::toString() const {
    return QStringLiteral("[%1] %2 (%3) — %4")
        .arg(QString::fromLatin1(describe(kind)), path, widgetClass, detail);
}

double relativeLuminance(const QColor& color) {
    const QColor rgb = color.toRgb();
    return 0.2126 * linearize(rgb.redF()) + 0.7152 * linearize(rgb.greenF()) +
           0.0722 * linearize(rgb.blueF());
}

double contrastRatio(const QColor& foreground, const QColor& background) {
    const double a = relativeLuminance(foreground);
    const double b = relativeLuminance(background);
    const double lighter = std::max(a, b);
    const double darker = std::min(a, b);
    return (lighter + 0.05) / (darker + 0.05);
}

std::vector<Finding> auditWidgetTree(QWidget* root) {
    std::vector<Finding> findings;
    if (root == nullptr) return findings;

    // 樣式表裡的顏色字面值。高對比模式下系統換的是 QPalette，樣式表不會跟著變，
    // 所以任何 #rrggbb 或 rgb() 都會固定蓋住系統色。palette(...) 形式是允許的，
    // 那正是「隨主題走」的寫法。
    static const QRegularExpression kColorLiteral(
        QStringLiteral("#[0-9a-fA-F]{3,8}\\b|\\brgba?\\s*\\("));

    const QList<QWidget*> widgets = root->findChildren<QWidget*>();
    QList<QWidget*> all = widgets;
    all.prepend(root);

    for (QWidget* widget : all) {
        if (widget == nullptr) continue;

        const QString sheet = widget->styleSheet();
        if (!sheet.isEmpty() && kColorLiteral.match(sheet).hasMatch()) {
            findings.push_back(makeFinding(FindingKind::HardcodedStyleSheetColor, widget,
                                           QStringLiteral("樣式表：%1").arg(sheet.simplified())));
        }

        if (hasQtInternalAncestor(widget) || isCompositeChild(widget)) continue;
        if (!isInteractive(widget)) continue;

        if (requiresKeyboardReach(widget) && widget->focusPolicy() == Qt::NoFocus) {
            findings.push_back(makeFinding(
                FindingKind::NotKeyboardReachable, widget,
                QStringLiteral("focusPolicy 為 NoFocus，且沒有選單或快捷鍵可以到達這個功能")));
        }

        if (widget->objectName().isEmpty() && !isFrameworkGenerated(widget)) {
            findings.push_back(makeFinding(
                FindingKind::MissingAutomationId, widget,
                QStringLiteral("沒有 objectName，UIA 的 AutomationId 會是空的")));
        }

        // 這一段是整個稽核的重點：問的是螢幕閱讀器實際拿到什麼，
        // 而不是我們設了什麼屬性。
        // 框架自動產生的元件（tabify 的 QMainWindowTabBar、工具列的延伸按鈕）
        // 我們既拿不到指標也設不了屬性，名稱由 Qt 依內容推導。
        // 對它們報缺失只會產生永遠修不掉的項目。
        if (isFrameworkGenerated(widget)) continue;

        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(widget);
        if (iface == nullptr) {
            findings.push_back(makeFinding(FindingKind::MissingAccessibleName, widget,
                                           QStringLiteral("沒有 QAccessibleInterface，"
                                                          "螢幕閱讀器看不到這個元件")));
            continue;
        }

        if (iface->text(QAccessible::Name).trimmed().isEmpty()) {
            findings.push_back(makeFinding(
                FindingKind::MissingAccessibleName, widget,
                QStringLiteral("QAccessible::Name 為空，UIA 只會念出角色名稱")));
        }

        const QAccessible::Role role = iface->role();
        if (role == QAccessible::NoRole || role == QAccessible::Client) {
            findings.push_back(makeFinding(
                FindingKind::UninformativeRole, widget,
                QStringLiteral("QAccessible 角色是 %1，輔助技術無法判斷用途")
                    .arg(role == QAccessible::NoRole ? QStringLiteral("NoRole")
                                                     : QStringLiteral("Client"))));
        }
    }

    return findings;
}

std::vector<Finding> auditPaletteContrast(const QPalette& palette, const QString& context) {
    std::vector<Finding> findings;

    // 只檢查真的會有文字疊在上面的角色配對。把所有角色兩兩相乘會產生一堆
    // 現實中不存在的組合，那種噪音會淹掉真正的問題。
    checkContrastPair(findings, palette.color(QPalette::WindowText),
                      palette.color(QPalette::Window), context, QStringLiteral("WindowText/Window"));
    checkContrastPair(findings, palette.color(QPalette::Text), palette.color(QPalette::Base),
                      context, QStringLiteral("Text/Base"));
    checkContrastPair(findings, palette.color(QPalette::ButtonText),
                      palette.color(QPalette::Button), context, QStringLiteral("ButtonText/Button"));
    checkContrastPair(findings, palette.color(QPalette::HighlightedText),
                      palette.color(QPalette::Highlight), context,
                      QStringLiteral("HighlightedText/Highlight"));
    checkContrastPair(findings, palette.color(QPalette::ToolTipText),
                      palette.color(QPalette::ToolTipBase), context,
                      QStringLiteral("ToolTipText/ToolTipBase"));

    return findings;
}

}  // namespace alioth::ui::a11y
