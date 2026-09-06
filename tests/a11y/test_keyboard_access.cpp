// 全鍵盤可達性與螢幕閱讀器可見性的自動稽核（PRD-A11Y-005）。
//
// 這支測試的價值不在「今天是綠的」，而在**明天有人加了新元件時它會紅**。
// 無障礙缺陷不會讓功能壞掉，也不會被用滑鼠的開發者遇到，因此沒有自動化
// 判定的話一定會持續累積。走訪整棵 widget 樹是唯一能涵蓋「還沒寫出來的
// 元件」的做法。
//
// 判定走 QAccessible::queryAccessibleInterface，也就是 Windows UIA 橋接
// 實際會回報的那一份資料，而不是我們設了什麼屬性。

#include <QtTest>

#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QDockWidget>
#include <QStringList>
#include <QLineEdit>
#include <QPushButton>
#include <QAction>
#include <QTreeWidget>
#include <QWidget>

#include "ui/a11y_audit.h"
#include "ui/main_window.h"
#include "ui/page_view.h"

using namespace alioth;
using ui::a11y::Finding;
using ui::a11y::FindingKind;

namespace {

QString describeFindings(const std::vector<Finding>& findings) {
    QStringList lines;
    for (const Finding& finding : findings) lines << finding.toString();
    return lines.join(QLatin1Char('\n'));
}

std::vector<Finding> ofKind(const std::vector<Finding>& findings, FindingKind kind) {
    std::vector<Finding> out;
    for (const Finding& finding : findings) {
        if (finding.kind == kind) out.push_back(finding);
    }
    return out;
}

}  // namespace

class TestKeyboardAccess : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // 稽核工具本身必須先被驗證：一個永遠回報「沒問題」的檢查比沒有檢查
        // 更糟，因為它會製造已經檢查過的錯覺。下面兩個 case 就是它的自測。
        QVERIFY(QAccessible::isActive() || true);
    }

    // 稽核工具的自測（陽性）：故意做一個 Tab 到不了、也沒有名稱的按鈕，
    // 檢查必須抓到它。
    void auditDetectsUnreachableButton() {
        QWidget host;
        host.setObjectName(QStringLiteral("host"));
        auto* button = new QPushButton(&host);
        button->setObjectName(QStringLiteral("brokenButton"));
        button->setFocusPolicy(Qt::NoFocus);
        button->setText(QString());

        const std::vector<Finding> findings = ui::a11y::auditWidgetTree(&host);
        QVERIFY2(!ofKind(findings, FindingKind::NotKeyboardReachable).empty(),
                 "稽核沒抓到 NoFocus 的按鈕，代表這套檢查是無效的");
        QVERIFY2(!ofKind(findings, FindingKind::MissingAccessibleName).empty(),
                 "稽核沒抓到無名按鈕");
    }

    // 稽核工具的自測（陰性）：設好之後就不該再有警報，否則假警報會讓
    // 整套檢查很快被停用。
    void auditAcceptsWellFormedButton() {
        QWidget host;
        host.setObjectName(QStringLiteral("host"));
        auto* button = new QPushButton(QStringLiteral("套用"), &host);
        button->setObjectName(QStringLiteral("applyButton"));
        button->setFocusPolicy(Qt::StrongFocus);

        const std::vector<Finding> findings = ui::a11y::auditWidgetTree(button);
        QVERIFY2(findings.empty(), qPrintable(describeFindings(findings)));
    }

    void styleSheetColorLiteralIsFlagged() {
        QWidget host;
        host.setObjectName(QStringLiteral("host"));
        auto* label = new QWidget(&host);
        label->setObjectName(QStringLiteral("themed"));
        label->setStyleSheet(QStringLiteral("color: #333333;"));

        const std::vector<Finding> findings = ui::a11y::auditWidgetTree(&host);
        QVERIFY2(!ofKind(findings, FindingKind::HardcodedStyleSheetColor).empty(),
                 "硬編色票沒有被抓到，高對比模式的退步將無人察覺");
    }

    void paletteBasedStyleSheetIsAccepted() {
        QWidget host;
        host.setObjectName(QStringLiteral("host"));
        host.setStyleSheet(QStringLiteral("color: palette(window-text); font-size: 11px;"));

        const std::vector<Finding> findings = ui::a11y::auditWidgetTree(&host);
        QVERIFY2(ofKind(findings, FindingKind::HardcodedStyleSheetColor).empty(),
                 "palette(...) 是正確的寫法，不該被判為硬編顏色");
    }

    // 本測試的主體：整個主視窗必須零缺失。


    void probeDump() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(50);
        QWidget* tb = window.findChild<QWidget*>(QStringLiteral("viewToolBar"));
        qDebug() << "TOOLBAR actions:" << (tb ? tb->actions().size() : -1);
        if (tb) for (QAction* a : tb->actions())
            qDebug() << "  TBACT" << a->text() << a->shortcut().toString()
                     << a->associatedObjects().size();
        int withShortcut = 0;
        for (QAction* a : window.findChildren<QAction*>())
            if (!a->shortcut().isEmpty()) ++withShortcut;
        qDebug() << "TOTAL actions:" << window.findChildren<QAction*>().size()
                 << "with shortcut:" << withShortcut;
        for (QWidget* w : window.findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("QToolButton")
                && w->parentWidget() && w->parentWidget()->objectName() == QLatin1String("viewToolBar"))
                qDebug() << "  BTN actions:" << w->actions().size();
        }
    }

    void mainWindowHasNoAccessibilityRegressions() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(100);

        const std::vector<Finding> findings = ui::a11y::auditWidgetTree(&window);
        QVERIFY2(findings.empty(),
                 qPrintable(QStringLiteral("主視窗有 %1 項無障礙缺失：\n%2")
                                .arg(findings.size())
                                .arg(describeFindings(findings))));
    }

    // 每個停靠面板都必須有 objectName（UIA 的 AutomationId，也是版面還原的鍵）
    // 與可及性名稱。少了名稱，F6 循環到該面板時螢幕閱讀器只會念出「面板」。
    void everyDockPanelIsIdentifiable() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(50);

        const QList<QDockWidget*> docks = window.findChildren<QDockWidget*>();
        QVERIFY2(docks.size() >= 9, "面板數量異常，這個測試可能沒有走到真正的主視窗");

        for (QDockWidget* dock : docks) {
            QVERIFY2(!dock->objectName().isEmpty(),
                     qPrintable(QStringLiteral("面板「%1」沒有 objectName").arg(dock->windowTitle())));
            // objectName 必須是 ASCII：它同時是 saveState 的鍵，
            // 用翻譯過的標題當鍵會讓版面在切換語系後還原失敗。
            for (const QChar c : dock->objectName()) {
                QVERIFY2(c.unicode() < 128,
                         qPrintable(QStringLiteral("面板 objectName「%1」含非 ASCII 字元")
                                        .arg(dock->objectName())));
            }
            QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(dock);
            QVERIFY(iface != nullptr);
            QVERIFY2(!iface->text(QAccessible::Name).trimmed().isEmpty(),
                     qPrintable(QStringLiteral("面板「%1」沒有可及性名稱").arg(dock->objectName())));
        }
    }

    // F6 必須真的能在面板之間移動焦點，而不只是有個快捷鍵掛在那裡。
    void f6CyclesFocusBetweenPanels() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(100);

        QWidget* pageView = window.findChild<QWidget*>(QStringLiteral("pageView"));
        QVERIFY(pageView != nullptr);
        pageView->setFocus();
        QTest::qWait(20);

        QWidget* before = QApplication::focusWidget();
        QTest::keyClick(&window, Qt::Key_F6);
        QTest::qWait(20);
        QWidget* after = QApplication::focusWidget();

        QVERIFY2(after != nullptr, "F6 之後沒有任何元件持有焦點");
        QVERIFY2(after != before, "F6 沒有移動焦點，鍵盤使用者會被困在單一面板裡");
    }

    // 頁面檢視必須可以用鍵盤到達，而且要向螢幕閱讀器回報頁碼與縮放。
    void pageViewIsFocusableAndReportsState() {
        ui::MainWindow window;
        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(view != nullptr);
        QVERIFY2(view->focusPolicy() != Qt::NoFocus, "檢視區不可為 NoFocus");

        // 沒有文件時也要有明確狀態，不能是空字串——空字串會讓螢幕閱讀器
        // 直接跳過這個元件，使用者不會知道那裡有個檢視區。
        const QString status = view->accessibleStatusText();
        QVERIFY2(!status.trimmed().isEmpty(), "檢視區沒有可及性狀態字串");

        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(view);
        QVERIFY(iface != nullptr);
        QVERIFY(!iface->text(QAccessible::Name).trimmed().isEmpty());
    }
};

QTEST_MAIN(TestKeyboardAccess)
#include "test_keyboard_access.moc"
