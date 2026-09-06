#pragma once

// 內容替代文字設定（PRD-A11Y-004）的寫入通道。
//
// 走物件層增量附加（ADR-002），不是因為 PDFium 的限制特別明顯——單純寫一個
// /Alt 字串鍵，FPDF_StructElement 系列 API 完全沒有 setter，所以這裡沒有
// 「PDFium 做得到但不精確」的問題，是「PDFium 完全做不到」。
//
// 只支援間接物件（objectNumber != 0）的結構元素：直接寫在父字典 /K 陣列裡的
// 結構元素沒有自己的物件編號，改寫它必須連同父物件一起重寫，而父物件是誰、
// 重寫會不會波及其他子節點，超出這個函式該管的範圍。struct_tree_reader 讀出的
// StructNode::objectNumber 為 0 時，呼叫端必須先擋下並提示使用者，
// 不能靜默失敗或誤改到別的物件。

#include <string>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct AltTextEditStatus {
    bool ok{false};
    std::string diagnostic;
};

// 設定或清除一個結構元素的 /Alt。altTextUtf8 為空字串時移除 /Alt 鍵
// （等同宣告「這個元素目前沒有替代文字」，而不是寫入一個空字串——
// 空字串在部分螢幕閱讀器上仍會被念成「無說明的圖片」，兩者不能混為一談，
// 移除鍵值讓 Tags／Order／Inspector 三個面板一致顯示「缺少替代文字」）。
[[nodiscard]] AltTextEditStatus setAlternateText(IncrementalAppender& appender,
                                                 int structElementObjectNumber,
                                                 const std::string& altTextUtf8);

}  // namespace alioth::engine::objects
