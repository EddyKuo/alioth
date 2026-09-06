#pragma once

// 表單欄位的建立規格（PRD-FORM-010 ~ 020，WBS 10）。
//
// 這是「要建立什麼欄位」的描述，與 engine/forms/form_types.h 的
// FormFieldInfo（「文件裡現在有什麼欄位」的快照）刻意分開。兩者長得像，
// 但方向相反：一個是輸入、一個是輸出，合併之後每個欄位都得標註
// 「這個成員在建立時有意義還是在讀取時有意義」，那種型別遲早會被誤用。
//
// PDFium 沒有建立表單欄位的公開 API，因此建立這條路一律走 engine/objects
// 的物件層通道（ADR-002）。這個檔案只描述規格，不碰位元組。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::engine::formbuild {

// PRD-FORM-010 ~ 020 明列的九種欄位。
//
// Date 與 Image 在 PDF 規格裡不是獨立的 /FT：
//   - Date 是文字欄位加上日期格式，Acrobat 用 /AA 掛 JavaScript 格式化器實作。
//     本產品零腳本執行（PRD §8.2），因此改以私有鍵記錄格式，
//     並在外觀串流上直接畫出已格式化的文字。詳見 form_field_writer.h。
//   - Image 是「只顯示圖示的按鈕」（/Ff Pushbutton + /MK /TP 1），
//     Acrobat 自己也是這樣實作的，不是我們的簡化。
enum class BuildFieldType : std::uint8_t {
    Text,
    CheckBox,
    RadioGroup,
    ComboBox,
    ListBox,
    PushButton,
    Date,
    Image,
    Signature,

    // 條碼欄位（PRD-FORM-025）。/FT 仍是 Tx——欄位持有的是文字值，只是
    // 外觀畫成 Code 128 而不是文字。與 Date 同理：這不是 PDF 規格裡的
    // 獨立欄位型別，是「文字欄位 + 特殊外觀」的組合。
    Barcode,
};

[[nodiscard]] const char* describe(BuildFieldType type) noexcept;

// 對應的 /FT 名稱（Tx / Btn / Ch / Sig）。
[[nodiscard]] const char* fieldTypeName(BuildFieldType type) noexcept;

// 文字對齊（/Q）。
enum class FieldAlignment : std::uint8_t { Left = 0, Center = 1, Right = 2 };

struct FieldColor {
    double r{0.0};
    double g{0.0};
    double b{0.0};
};

// 外觀特徵（/MK）。Acrobat 的欄位屬性面板讀的就是這個字典；
// 沒有它時欄位仍然可填，但邊框與底色會完全消失，使用者看不出哪裡可以點。
struct FieldAppearanceCharacteristics {
    std::optional<FieldColor> backgroundColor{};  // /BG
    std::optional<FieldColor> borderColor{};      // /BC
    double borderWidth{1.0};                      // /BS /W
    bool dashedBorder{false};                     // /BS /S /D
    std::string caption;                          // /CA，按鈕的文字
};

// 單選群組的一顆按鈕。單選在 PDF 裡是「一個欄位、多個 widget」，
// 每個 widget 有自己的 /Rect 與外觀狀態名（匯出值）。
struct RadioOption {
    std::string exportValue;  // 外觀狀態名，必須是合法的 PDF 名稱
    domain::RectF rectPt{};
};

struct FieldDefinition {
    BuildFieldType type{BuildFieldType::Text};

    // 完整欄位名（/T）。允許以點分層（"address.city"），
    // Fields 面板會據此建樹（PRD-FORM-021）。
    std::string name;

    std::string alternateName;  // /TU，無障礙朗讀與滑鼠提示
    std::string mappingName;    // /TM，匯出資料時的鍵名

    std::int32_t pageIndex{0};
    domain::RectF rectPt{};  // 頁面空間；RadioGroup 忽略此欄，改用 options

    std::string value;         // /V 的初始值
    bool checked{false};       // CheckBox 的初始狀態
    std::string exportValue{"Yes"};  // CheckBox 的勾選狀態名

    std::vector<std::string> options;   // ComboBox / ListBox 的 /Opt
    std::vector<RadioOption> radios;    // RadioGroup 的各顆按鈕

    // 旗標（/Ff）。與 forms::FormFieldFlags 分開，因為建立端需要的旗標
    // 比讀取端多（comb、fileSelect、sort…），而讀取端只暴露 UI 用得到的那些。
    bool readOnly{false};
    bool required{false};
    bool noExport{false};
    bool multiline{false};
    bool password{false};
    bool comb{false};
    bool comboEditable{false};
    bool multiSelect{false};
    bool sortOptions{false};

    std::int32_t maxLength{0};  // /MaxLen，0 代表不限；comb 需要它才有意義

    double fontSize{0.0};  // 0 代表自動（/DA 寫 0 Tf，由檢視器決定）
    FieldColor textColor{};
    FieldAlignment alignment{FieldAlignment::Left};

    FieldAppearanceCharacteristics appearance{};

    // 日期欄位的格式字串（例如 "yyyy/mm/dd"）。零腳本執行下無法用
    // Acrobat 的 /AA JavaScript 格式化器，因此以私有鍵保存並由本產品自行套用。
    std::string dateFormat{"yyyy/mm/dd"};

    // 計算式（PRD-FORM-022）。與日期格式同理，以私有鍵保存，
    // 不寫 /AA /C JavaScript——那正是 PRD §2.1 第 5 項排除的東西。
    std::string calculation;
};

// 把旗標欄位算成 /Ff 的位元值。位元編號依 ISO 32000-2 §12.7.4，
// 從 1 起算，因此值是 1 << (bit - 1)；直接寫十進位魔術數字是這裡最常見的錯誤。
[[nodiscard]] std::int64_t computeFieldFlags(const FieldDefinition& definition);

// 建立前的規格檢查。回傳空字串代表通過。
//
// 錯誤在這裡就攔下來而不是等寫完檔案才發現（IL-4）：一個 /Rect 寬度為零的
// 欄位在 Acrobat 裡是看不見也點不到的，而檔案結構完全合法，qpdf 也不會抱怨。
[[nodiscard]] std::string validate(const FieldDefinition& definition);

// 複製欄位（PRD-FORM-011~020「複製欄位」）。
//
// 刻意不自動生成新名稱：自動生成的名稱容易撞到既有欄位，或產生使用者看不懂
// 的字尾（"_copy1_copy2"），決定權留給呼叫端（通常是 UI 對話框，且它手上有
// FormFieldWriter::existingFieldNames() 可以先檢查撞名）。
//
// offsetXPt/offsetYPt 套用在 rectPt（或 RadioGroup 的每一顆
// radios[i].rectPt）上，讓複製出來的欄位預設不會與原欄位完全重疊——
// 重疊的話使用者在畫面上看不出多了一個欄位，會以為複製沒有生效。
[[nodiscard]] FieldDefinition duplicateFieldDefinition(const FieldDefinition& source,
                                                       std::string newName,
                                                       double offsetXPt = 12.0,
                                                       double offsetYPt = -12.0);

}  // namespace alioth::engine::formbuild
