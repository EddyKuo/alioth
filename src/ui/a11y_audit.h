#pragma once

// 無障礙自動稽核（PRD-A11Y-005）。
//
// 這個模組存在的理由不是「檢查一次」，而是**擋住退步**。無障礙是所有品質
// 屬性裡最容易靜默腐蝕的一項：沒有人在日常開發中會注意到新加的按鈕
// Tab 不到，因為開發者用滑鼠。等到有人回報時，那個按鈕已經在十個版本裡了。
// 因此這裡把判定寫成可執行的函式，由 tests/a11y 在每次建置時走訪整棵
// widget 樹，新元件一漏就紅。
//
// 判定刻意走 QAccessible::queryAccessibleInterface 而不是直接讀
// widget->accessibleName()：螢幕閱讀器看到的是 QAccessibleInterface 回報的
// 名稱，而 Qt 會替按鈕之類的元件從 text() 推導。直接讀屬性會把「Qt 已經
// 推導出正確名稱」誤判成缺失，而那種假警報會讓整套檢查很快被停用。
//
// 這個模組屬於呈現層，不呼叫 PDFium，也不需要文件。

#include <QColor>
#include <QString>

#include <vector>

class QPalette;
class QWidget;

namespace alioth::ui::a11y {

enum class FindingKind {
    // 可互動但 focusPolicy 為 NoFocus：鍵盤永遠到不了。
    NotKeyboardReachable,
    // 螢幕閱讀器取不到名稱：UIA 會念成「按鈕」而不是「套用螢光筆」。
    MissingAccessibleName,
    // 沒有 objectName：UIA 的 AutomationId 空白，UI 自動化與測試無法定位。
    MissingAutomationId,
    // QAccessible 角色是泛用的 Client/NoRole：輔助技術不知道這是什麼東西。
    UninformativeRole,
    // styleSheet 裡有硬編色票：Windows 高對比模式下會蓋掉系統色。
    HardcodedStyleSheetColor,
    // 前景與背景對比未達 WCAG AA 的 4.5:1。
    InsufficientContrast,
};

[[nodiscard]] const char* describe(FindingKind kind) noexcept;

struct Finding {
    FindingKind kind{FindingKind::MissingAccessibleName};
    QString widgetClass;   // metaObject()->className()
    QString objectName;
    QString path;          // 從根到該元件的 objectName/class 路徑，用來定位
    QString detail;

    [[nodiscard]] QString toString() const;
};

// WCAG 2.1 AA：一般文字 4.5:1，大型文字（≥18pt 或粗體 ≥14pt）3:1。
inline constexpr double kContrastAaNormal = 4.5;
inline constexpr double kContrastAaLarge = 3.0;

// WCAG 2.1 定義的相對亮度與對比比值。輸入須是 sRGB。
[[nodiscard]] double relativeLuminance(const QColor& color);
[[nodiscard]] double contrastRatio(const QColor& foreground, const QColor& background);

// 走訪 root 底下所有的 QWidget（含 root），回傳所有缺失。
//
// 隱藏的元件仍然檢查：面板預設收合不代表使用者不會打開它，
// 而「只有打開過的面板才檢查」會讓覆蓋率取決於測試碰巧開了哪幾個。
[[nodiscard]] std::vector<Finding> auditWidgetTree(QWidget* root);

// 檢查一組 QPalette 角色配對的對比。這是高對比模式的可執行判定：
// 只要程式不硬編顏色，這個檢查在使用者切到高對比佈景時仍然成立，
// 因為系統會把高對比色塞進同一組角色。
[[nodiscard]] std::vector<Finding> auditPaletteContrast(const QPalette& palette,
                                                        const QString& context);

}  // namespace alioth::ui::a11y
