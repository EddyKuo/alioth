#pragma once

// 表單子系統的資料模型（WBS 6.1–6.4，PRD-FORM-001 ~ 005）。
//
// 這些型別刻意留在引擎轉接層而不是領域層：它們與 PDFium 的欄位分類一對一對應，
// 一旦 PDFium 升版改變分類，變動應該被擋在轉接層裡，而不是傳染到領域模型。
//
// 只依賴 domain::RectF 這類純幾何型別，不引入 PDFium 標頭——PDFium 標頭
// 不得外洩到引擎轉接層以外，這是分層規則的實際執行手段。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::engine::forms {

// 對應 PDFium 的 FORMTYPE_*。分開定義而不是直接用整數，
// 是因為 XFA 兩種型別的處置不同（PRD-FORM-003），呼叫端必須被迫分辨。
enum class FormType {
    None,
    AcroForm,
    XfaFull,        // 動態 XFA：版面完全由 XFA 決定
    XfaForeground,  // XFAF：AcroForm 為主、XFA 疊加
};

enum class FormFieldType {
    Unknown,
    PushButton,
    CheckBox,
    RadioButton,
    ComboBox,
    ListBox,
    TextField,
    Signature,
    Xfa,  // XFA 專屬型別。本建置關閉 XFA，出現即代表文件超出支援範圍
};

[[nodiscard]] const char* describe(FormFieldType type) noexcept;
[[nodiscard]] const char* describe(FormType type) noexcept;

// 欄位旗標。位元值取自 PDF 規格的 /Ff，這裡重新命名以免呼叫端硬寫魔術數字。
struct FormFieldFlags {
    bool readOnly{false};
    bool required{false};
    bool noExport{false};
    bool multiline{false};
    bool password{false};
    bool comboEditable{false};
    bool multiSelect{false};
};

// 單一欄位（實際上是單一 widget）的快照。
//
// PDF 的一個欄位可以有多個 widget（單選群組是典型例子），因此 name 不唯一。
// annotIndex 才是頁內的唯一鍵，所有寫入操作都以 (pageIndex, annotIndex) 定位，
// 用名字定位在單選群組上會改錯按鈕。
struct FormFieldInfo {
    std::int32_t pageIndex{0};
    std::int32_t annotIndex{0};
    std::string name;
    std::string alternateName;  // /TU，無障礙朗讀與提示用
    FormFieldType type{FormFieldType::Unknown};
    std::string value;               // /V 的文字表示
    std::string exportValue;         // 核取／單選的匯出值（/AP /N 的狀態名）
    std::vector<std::string> options;         // /Opt
    std::vector<std::int32_t> selectedIndices;
    bool checked{false};
    FormFieldFlags flags{};
    domain::RectF rectPt{};  // 頁面空間（原點左下、Y 向上）

    [[nodiscard]] bool isChoice() const noexcept {
        return type == FormFieldType::ComboBox || type == FormFieldType::ListBox;
    }
    [[nodiscard]] bool isButton() const noexcept {
        return type == FormFieldType::CheckBox || type == FormFieldType::RadioButton ||
               type == FormFieldType::PushButton;
    }
};

// 辨識表單的結果（PRD-FORM-005）。
//
// 「沒有表單」與「有表單但這一頁沒有欄位」是兩件事，混在一起會讓使用者
// 在多頁文件上以為表單不見了，所以兩個計數分開回報。
struct FormIdentification {
    FormType formType{FormType::None};
    bool hasAcroFormDictionary{false};
    std::int32_t fieldCount{0};
    std::int32_t pagesWithFields{0};
    std::vector<std::int32_t> pageIndicesWithFields;

    [[nodiscard]] bool isFillable() const noexcept {
        return formType == FormType::AcroForm && fieldCount > 0;
    }
};

// XFA 偵測與降級提示（PRD-FORM-003 / WBS 6.4）。
//
// PRD §13 要求提示不得誤導：本建置關閉 XFA，能顯示的只有 AcroForm 後備內容。
// 因此訊息必須同時說出「顯示的是什麼」與「沒顯示的是什麼」，
// 不能只寫一句「不支援」——使用者會以為檔案壞了。
struct XfaReport {
    bool present{false};
    bool dynamic{false};              // FORMTYPE_XFA_FULL，版面完全由 XFA 決定
    bool acroFormFallbackUsable{false};  // 後備 AcroForm 欄位是否存在且可填
    std::string message;              // 繁體中文，可直接顯示於狀態列
};

[[nodiscard]] XfaReport describeXfa(FormType type, bool acroFormFieldsExist);

}  // namespace alioth::engine::forms
