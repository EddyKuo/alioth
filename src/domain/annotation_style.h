#pragma once

// Comment Styles 樣式面板的純邏輯核心(PRD-ANN-029)。
//
// 樣式只描述「外觀相關」的欄位:顏色、填色、透明度、邊框。刻意不包含
// 內容文字、作者、位置——套用樣式不該覆蓋使用者已經寫好的評註內容,
// 也不該把註解搬到別的地方。旗標(鎖定/隱藏/列印)也不在樣式裡:
// PRD-ANN-012 是另一個獨立的操作面,套用視覺樣式不該連帶改變可見度或
// 列印行為。
//
// 純函數,只讀寫 domain::Annotation 既有欄位,不修改 domain/annotation.h。

#include <optional>

#include "domain/annotation.h"

namespace alioth::domain {

struct CommentStyle {
    ColorRgb color{1.0, 0.85, 0.0};
    std::optional<ColorRgb> interiorColor{};
    double opacity{1.0};
    BorderStyle border{};

    friend bool operator==(const CommentStyle&, const CommentStyle&) = default;
};

// 從一則既有註解擷取出可另存為樣式的欄位,供「另存為目前樣式」使用。
[[nodiscard]] inline CommentStyle extractStyle(const Annotation& annotation) noexcept {
    CommentStyle style{};
    style.color = annotation.color;
    style.interiorColor = annotation.interiorColor;
    style.opacity = annotation.opacity;
    style.border = annotation.border;
    return style;
}

// 把樣式套用到一則註解,回傳套用後的新副本;其餘欄位(內容、作者、位置、
// 幾何、旗標、回覆串)完全不動。
[[nodiscard]] inline Annotation applyStyle(const Annotation& annotation, const CommentStyle& style) {
    Annotation out = annotation;
    out.color = style.color;
    out.interiorColor = style.interiorColor;
    out.opacity = style.opacity;
    out.border = style.border;
    return out;
}

// 「設為預設樣式」情境下常用的比較:兩則註解的視覺樣式是否相同,
// 用來判斷面板該不該顯示「已套用」的勾選狀態。
[[nodiscard]] inline bool hasStyle(const Annotation& annotation, const CommentStyle& style) noexcept {
    return extractStyle(annotation) == style;
}

}  // namespace alioth::domain
