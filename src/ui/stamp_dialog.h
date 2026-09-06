#pragma once

// 把文字戳記寫進文件（PRD-PAGE-004）：頁首頁尾、浮水印、Bates 編號。
//
// 三者在使用者眼中是三件事，在實作上是同一件事——「在每頁的某個位置畫一段
// 支援符號展開的文字」。所以這裡是**一個對話框三種預設**，不是三個對話框：
// 三份幾乎一樣的程式碼會慢慢分岔，而分岔的那一份遲早會少支援一個符號。
//
// 預設只決定「開啟時長什麼樣」，之後每一個欄位都還能改。把預設做成不可逾越的
// 模式，等於逼使用者為了「浮水印但放在頁尾」這種需求去找別的功能。

#include <QDialog>

#include "app/stamp_service.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace alioth::ui {

class StampDialog : public QDialog {
    Q_OBJECT

public:
    // 開啟時要套用哪一組預設。
    enum class Preset { HeaderFooter, Watermark, Bates };

    StampDialog(Preset preset, QWidget* parent = nullptr);

    // 除了 path 之外的欄位；呼叫端補上路徑。
    //
    // totalPages 用來驗證使用者輸入的頁碼範圍。留空代表全部頁面——
    // 這個預設與「刪除頁面」相反（那裡留空代表取消），因為兩個操作的
    // 風險不對稱：多蓋一頁戳記可以復原，多刪一頁不行。
    [[nodiscard]] app::StampRequest request(int totalPages) const;

    // 使用者輸入的頁碼範圍是否合法。不合法時呼叫端要擋下來並說明，
    // 而不是靜靜地當成「全部頁面」——那會在整份文件上蓋滿戳記。
    [[nodiscard]] bool pageRangeIsValid(int totalPages) const;

private:
    void applyPreset(Preset preset);
    void updateBatesEnabled();

    QLineEdit* pageRange_{nullptr};
    QLineEdit* text_{nullptr};
    QComboBox* anchor_{nullptr};
    QDoubleSpinBox* fontSize_{nullptr};
    QDoubleSpinBox* margin_{nullptr};
    QCheckBox* useBates_{nullptr};
    QLineEdit* batesPrefix_{nullptr};
    QLineEdit* batesSuffix_{nullptr};
    QSpinBox* batesStart_{nullptr};
    QSpinBox* batesDigits_{nullptr};
    QSpinBox* batesIncrement_{nullptr};
};

}  // namespace alioth::ui
