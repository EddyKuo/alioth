// 對比與高對比模式的可執行判定（PRD-A11Y-005，WCAG 2.1 AA）。
//
// 「高對比模式下不可以有硬編色票蓋掉系統色」聽起來像一條靠自律遵守的規則，
// 但它其實可以驗：只要程式的顏色全部取自 QPalette 角色，把佈景換成
// Windows 高對比的配色之後，所有文字對背景的對比仍然成立。反之，只要有一處
// 硬編，換色之後就會出現對比不足的配對。
//
// 因此這支測試做兩件事：
//   1. 驗對比計算本身正確（用 WCAG 規格書列出的已知數值）
//   2. 把主視窗套上模擬的 Windows 高對比佈景，再走一次稽核

#include <QtTest>

#include <QApplication>
#include <QPalette>
#include <QStringList>

#include <algorithm>

#include "ui/a11y_audit.h"
#include "ui/main_window.h"

using namespace alioth;
using ui::a11y::contrastRatio;
using ui::a11y::Finding;
using ui::a11y::FindingKind;

namespace {

// Windows 高對比黑（High Contrast Black）的近似配色。用它而不是直接呼叫
// 系統 API，理由是測試要能在 CI 的無頭機器上跑，而且不受跑測試那台機器
// 目前的佈景設定影響——測試結果取決於執行環境是最難重現的失敗模式。
QPalette highContrastBlackPalette() {
    QPalette palette;
    const QColor background(0x00, 0x00, 0x00);
    const QColor text(0xFF, 0xFF, 0xFF);
    const QColor highlight(0x1A, 0xEB, 0xFF);
    const QColor highlightText(0x00, 0x00, 0x00);

    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, background);
    palette.setColor(QPalette::AlternateBase, background);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, background);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, highlightText);
    palette.setColor(QPalette::ToolTipBase, background);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Mid, QColor(0x3F, 0x3F, 0x3F));
    palette.setColor(QPalette::Dark, QColor(0x1F, 0x1F, 0x1F));
    return palette;
}

QString describe(const std::vector<Finding>& findings) {
    QStringList lines;
    for (const Finding& finding : findings) lines << finding.toString();
    return lines.join(QLatin1Char('\n'));
}

}  // namespace

class TestContrast : public QObject {
    Q_OBJECT

private slots:
    // 先驗計算本身。算錯的對比檢查會給出錯誤的安全感，比沒有檢查更危險。
    void contrastRatioMatchesWcagReferenceValues() {
        // 黑白是 21:1，這是 WCAG 定義的上限。
        QVERIFY(qAbs(contrastRatio(QColor(Qt::black), QColor(Qt::white)) - 21.0) < 0.01);
        // 同色是 1:1。
        QVERIFY(qAbs(contrastRatio(QColor(0x12, 0x34, 0x56), QColor(0x12, 0x34, 0x56)) - 1.0) <
                0.001);
        // 對稱：前景背景互換不改變比值。
        const double forward = contrastRatio(QColor(0x77, 0x77, 0x77), QColor(Qt::white));
        const double backward = contrastRatio(QColor(Qt::white), QColor(0x77, 0x77, 0x77));
        QVERIFY(qAbs(forward - backward) < 1e-9);
        // #767676 是 WCAG 常被引用的白底 AA 臨界值（4.54:1）。
        const double borderline = contrastRatio(QColor(0x76, 0x76, 0x76), QColor(Qt::white));
        QVERIFY2(borderline >= 4.5 && borderline < 4.6,
                 qPrintable(QStringLiteral("#767676 對白底應為約 4.54:1，實得 %1")
                                .arg(borderline)));
    }

    // 這一項記錄的是簽章面板硬編綠色的實際問題：它在白底可讀，在高對比黑底
    // 只有 2.6:1。程式因此不能無條件使用它——signature_panel 的
    // readableTrustColor 就是為了這個而存在。
    void hardcodedTrustGreenFailsOnBlackBackground() {
        const QColor trustGreen(0x1B, 0x7F, 0x3B);
        QVERIFY(contrastRatio(trustGreen, QColor(Qt::white)) >= 4.5);
        QVERIFY2(contrastRatio(trustGreen, QColor(Qt::black)) < 4.5,
                 "若這一行變綠，代表色票被改過，signature_panel 的退回邏輯需要重新評估");
    }

    // 已知的平台缺口，刻意寫成測試而不是留在文件裡。
    //
    // Qt 預設佈景的選取色 HighlightedText/Highlight 是白字配 #308cc6，
    // 只有 3.69:1，未達 WCAG AA 的 4.5:1。這是 Qt 自己的配色，不是本專案的
    // 硬編顏色，而覆寫系統選取色會帶來更大的問題：使用者在作業系統設定的
    // 選取色是全域偏好，單一應用程式自行改掉會與其他程式不一致，
    // 高對比模式下更會直接把系統色蓋掉——那正是我們要避免的事。
    //
    // 因此這裡不假裝它通過，而是把它釘住：唯一允許的失敗是選取色這一對，
    // 而且必須仍達到 3:1（WCAG 對大型文字與非文字元素的門檻）。
    // 只要 Qt 改善它、或我們自己引入了別的低對比配對，這支測試就會紅。
    void defaultPaletteOnlyFailsOnPlatformSelectionColour() {
        const std::vector<Finding> findings =
            ui::a11y::auditPaletteContrast(QApplication::palette(), QStringLiteral("預設佈景"));

        for (const Finding& finding : findings) {
            QVERIFY2(finding.detail.contains(QStringLiteral("HighlightedText/Highlight")),
                     qPrintable(QStringLiteral("預設佈景出現非預期的低對比配對：\n%1")
                                    .arg(describe(findings))));
        }

        const QPalette palette = QApplication::palette();
        const double selection = contrastRatio(palette.color(QPalette::HighlightedText),
                                               palette.color(QPalette::Highlight));
        QVERIFY2(selection >= 3.0,
                 qPrintable(QStringLiteral("選取色連 3:1 都不到（%1:1），必須自訂佈景補救")
                                .arg(selection)));
    }

    void highContrastPalettePassesAa() {
        const std::vector<Finding> findings = ui::a11y::auditPaletteContrast(
            highContrastBlackPalette(), QStringLiteral("Windows 高對比黑"));
        QVERIFY2(findings.empty(), qPrintable(describe(findings)));
    }

    // 主視窗在高對比佈景下不得出現任何硬編色票。
    //
    // 這是「高對比模式不可被蓋掉」這條規則唯一可自動化的形式：
    // 我們沒辦法在無頭機器上截圖比對，但可以確認沒有任何一處樣式表
    // 把顏色寫死——寫死的地方正是換佈景時不會跟著變的地方。
    void mainWindowUsesNoHardcodedColorsUnderHighContrast() {
        ui::MainWindow window;
        window.setPalette(highContrastBlackPalette());
        window.show();
        QTest::qWait(100);

        std::vector<Finding> findings = ui::a11y::auditWidgetTree(&window);
        // 只看顏色相關的兩類；鍵盤與命名由 test_keyboard_access 負責，
        // 兩支測試各自失敗才看得出是哪一類退步。
        findings.erase(std::remove_if(findings.begin(), findings.end(),
                                      [](const Finding& finding) {
                                          return finding.kind !=
                                                     FindingKind::HardcodedStyleSheetColor &&
                                                 finding.kind != FindingKind::InsufficientContrast;
                                      }),
                       findings.end());
        QVERIFY2(findings.empty(), qPrintable(describe(findings)));

        // 視窗自己的佈景也要通過角色配對檢查。
        const std::vector<Finding> paletteFindings =
            ui::a11y::auditPaletteContrast(window.palette(), QStringLiteral("主視窗"));
        QVERIFY2(paletteFindings.empty(), qPrintable(describe(paletteFindings)));
    }
};

QTEST_MAIN(TestContrast)
#include "test_contrast.moc"
