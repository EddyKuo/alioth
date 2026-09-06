#pragma once

// 表單欄位的外觀串流產生器（PRD-FORM-010 ~ 020 的 /AP 部分）。
//
// 為什麼欄位一定要自產 /AP：PDF 規格允許以 /NeedAppearances true 要求檢視器
// 自行產生外觀，但那是「請求」而不是「保證」——Acrobat 會做，Chrome 的 PDF
// 檢視器與 macOS 預覽多半不會，結果是欄位在那些檢視器上完全不顯示。
// 這與 CLAUDE.md「註解必須自產外觀串流」是同一條理由，只是對象換成 widget。
//
// 與 engine/annotations/appearance_stream.h 同一個立場：純函數、不連結 PDFium、
// 不碰檔案系統，輸入是規格、輸出是位元組，因此可以逐運算子驗證。
//
// 座標約定：/BBox 一律是 [0 0 寬 高]（原點在 widget 左下），/Matrix 為單位矩陣。
// 這樣 §12.5.5 的外觀對映演算法退化成純平移，任何偏移都是我們自己算錯的。

#include <set>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "engine/formbuild/field_definition.h"

namespace alioth::engine::formbuild {

// 外觀狀態。核取與單選的 /AP /N 是一個「狀態名 → 串流」的字典，
// 其餘欄位只有單一串流；用同一個結構表達兩者，寫入層就不必分支。
struct FieldAppearanceState {
    std::string stateName;  // 空字串代表無狀態（單一串流）
    std::string content;    // 內容串流位元組，恆為 7-bit ASCII
    bool needsFont{false};  // 是否引用 /Helv，決定要不要寫 /Resources /Font
};

struct FieldAppearance {
    bool valid{false};
    std::string diagnostic;

    domain::RectF bbox{};  // 恆為 [0 0 寬 高]
    std::vector<FieldAppearanceState> states;

    // 是否引用 /CJK，以及用到哪些字（ADR-007）。
    //
    // 放在整份外觀而不是個別 state：一個欄位的所有狀態共用同一份字型物件，
    // 逐狀態各嵌一份會讓同一個欄位重複內嵌好幾份相同的子集。
    //
    // 兩者必須成對：引用了 /CJK 卻沒註冊，欄位的中文會整段消失而不是亂碼，
    // 而且不會有任何錯誤訊息。
    bool needsCjkFont{false};
    std::set<char32_t> cjkCodepoints{};
};

// 產生欄位 widget 的外觀。radioIndex 只在 RadioGroup 時有意義，
// 指的是 definition.radios 裡的第幾顆按鈕。
[[nodiscard]] FieldAppearance generateFieldAppearance(const FieldDefinition& definition,
                                                      std::size_t radioIndex = 0);

// 檢查 UTF-8 文字是否整段可用 WinAnsi/Helvetica 畫出（僅可列印 ASCII 加上
// \n / \r）。
//
// 這與 engine/annotations/text_layout.h 是同一條策略：ADR-007 之後 CJK 走內嵌
// 子集，因此「畫得出來」的範圍是可列印 ASCII 加上子集裡有字形的碼點；
// 落在範圍外的字元一律明確失敗，不輸出殘缺的部分內容。
//
// 舊版本會靜默丟棄非 ASCII 字元、只畫出剩下的部分——這會造成 /V 有完整的
// UTF-16 值，但外觀串流只畫出被截斷後的殘影甚至整段空白，使用者以為
// 資料沒存進去。ok == false 時 text 必為空字串，呼叫端不得使用它。
struct AsciiFold {
    bool ok{false};
    std::string text;
};

[[nodiscard]] AsciiFold foldToWinAnsi(const std::string& utf8);

// Helvetica 的字寬估算（單位為 1/1000 em）。用於置中與靠右對齊。
//
// 沒有內嵌字型度量表，因此這是估算而不是精確值；對齊誤差在數個點的量級，
// 不影響可用性。要精確就得內嵌字型度量，那屬於字型策略定案後的工作。
[[nodiscard]] double estimateHelveticaWidth(const std::string& asciiText, double fontSize);

}  // namespace alioth::engine::formbuild
