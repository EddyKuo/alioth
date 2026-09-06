#pragma once

// 可調整游標大小（PRD-UI-018）。
//
// Windows 的「指標大小」是系統設定，套用在系統游標（QCursor 的內建 Shape，
// 例如 Qt::ArrowCursor、Qt::IBeamCursor）上時，作業系統會自動依那個設定縮放，
// 應用程式什麼都不用做。但本專案的形狀註解工具（矩形／橢圓等）用的十字準星
// 目前是 Qt 內建的 Qt::CrossCursor——一旦哪天需要一個「系統沒有、我們自己畫」
// 的游標（本模組先把它做出來，供 Qt::CrossCursor 換成可縮放版本使用），
// 作業系統就沒有辦法幫忙縮放一張我們自己畫的點陣圖：那張圖必須自己
// 依使用者選的尺寸與目前螢幕 DPI 重畫。
//
// 因此這裡採「資料描述的游標集合」：每個尺寸等級對應一個明確的倍率，
// 而不是散在呼叫端各自寫死一個數字。

#include <QCursor>
#include <QString>

class QPixmap;

namespace alioth::app {

enum class CursorSizeLevel {
    Normal,      // 1.0x，系統預設
    Large,       // 1.5x
    ExtraLarge,  // 2.0x
    Huge,        // 3.0x，對應 Windows「指標大小」滑桿最大檔位附近的視覺量級
};

class CursorScale {
public:
    // 倍率取自 Windows「變更指標大小」的常見檔位（1x/1.5x/2x/3x），
    // 不是隨意挑的數字，讓使用者在系統設定與本程式看到的「大一號」量感一致。
    [[nodiscard]] static double scaleFactor(CursorSizeLevel level);

    // 與 QSettings 之間的整數對應（0-3）。超出範圍一律夾回 Normal——
    // 設定檔被手動改壞或跨版本欄位增減時，不該讓游標大小的讀取直接崩潰。
    [[nodiscard]] static CursorSizeLevel fromSettingsValue(int value);
    [[nodiscard]] static int toSettingsValue(CursorSizeLevel level);

    [[nodiscard]] static QString displayName(CursorSizeLevel level);

    // 畫一個十字準星游標圖樣，依 level 與螢幕的 devicePixelRatio 縮放。
    // 熱點固定在圖樣正中央——十字準星的使用情境（框選矩形/橢圓起點）要求
    // 熱點必須精準對到使用者想標記的那個像素，偏移熱點會讓畫出來的形狀
    // 跟手指/游標實際位置系統性地錯開。
    [[nodiscard]] static QCursor buildCrosshairCursor(CursorSizeLevel level,
                                                       qreal devicePixelRatio);

private:
    // 基準尺寸（裝置獨立像素，96 DPI、Normal 等級）。
    static constexpr int kBaseSizePx = 24;
};

}  // namespace alioth::app
