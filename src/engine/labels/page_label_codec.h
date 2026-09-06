#pragma once

// /PageLabels 的讀寫（PRD-PAGE-013）。
//
// 走物件層而不是 PDFium：PDFium 有 FPDF_GetPageLabel 可以讀單頁的標籤字串，
// 但拿不到「範圍是怎麼定義的」——樣式、前綴、起始編號一個都取不到，
// 而編輯對話框需要的正是那些。寫入端 PDFium 更是完全沒有 API。
//
// 因此這個 target 不連結 PDFium，也不需要文件把手或專屬執行緒。
//
// 寫入一律走增量附加：/PageLabels 掛在 catalog 上，改動 catalog 是覆寫既有物件，
// 但附加式寫入不動原檔位元組，已簽章文件的簽章仍然有效（新內容會被
// 正確回報為未受簽章涵蓋）。

#include <string>
#include <vector>

#include "domain/page_labels.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::labels {

// 讀出 /PageLabels 數字樹。沒有這個鍵時回傳空的 map（不是錯誤——
// 多數 PDF 根本沒有頁面標籤，那時每頁的標籤就是它的頁碼）。
[[nodiscard]] domain::PageLabelMap readPageLabels(const objects::PdfSourceDocument& source);

struct PageLabelWriteResult {
    bool ok{false};
    std::string diagnostic;
    std::size_t rangeCount{0};
};

// 整棵換掉 /PageLabels。合併語意不提供：頁面標籤是一組互相依賴的範圍
// （每段的結束由下一段的起點決定），逐段合併會產生使用者沒要求的中間狀態。
// 編輯對話框的作法是讀出全部、在記憶體裡改、整份寫回。
//
// 空的 ranges 代表移除頁面標籤：輸出 /PageLabels << /Nums [] >>，
// 而不是把鍵刪掉——附加式寫入不刪既有物件，留一棵空樹是唯一乾淨的表達。
[[nodiscard]] PageLabelWriteResult writePageLabels(objects::IncrementalAppender& appender,
                                                   const domain::PageLabelMap& labels);

}  // namespace alioth::engine::labels
