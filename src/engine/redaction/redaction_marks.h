#pragma once

// 塗黑標記階段（PRD-ANN-033 的產出、PRD-ANN-032 的前一步，WBS 11）。
//
// 標記只是一則 /Redact 註解（ISO 32000-2 §12.5.6.23）。它**不會**移除任何內容，
// 因此走的是與其他註解一樣的增量附加通道：原檔位元組原封不動，刪掉註解就等於
// 回到標記前的狀態，既有數位簽章也仍然是「有效，簽章後有變更」。
//
// 這正是兩階段語意在實作上的分界線：標記走 IncrementalAppender（可逆），
// 套用走 PdfDocumentRewriter（不可逆、整份重寫）。兩者用不同的寫入通道，
// 而不是同一條通道上的一個旗標——旗標會被誤設，通道不會。

#include <string>
#include <vector>

#include "domain/redaction.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::redaction {

struct MarkWriteResult {
    bool ok{false};
    std::string diagnostic{};
    std::vector<int> annotationObjects{};  // 每則標記對應的註解物件編號
};

// 把標記寫成 /Redact 註解並掛上各頁的 /Annots。
[[nodiscard]] MarkWriteResult writeRedactionMarks(objects::IncrementalAppender& appender,
                                                  const domain::RedactionMarkSet& marks);

// 一次完成「開檔 → 寫標記 → 產生位元組」。回傳的仍是增量附加的輸出。
[[nodiscard]] objects::BuildResult markRedactions(std::string sourceBytes,
                                                  const domain::RedactionMarkSet& marks,
                                                  std::string* diagnostic = nullptr);

// 讀回文件中既有的 /Redact 標記。套用階段與「復原標記」都需要它。
[[nodiscard]] domain::RedactionMarkSet readRedactionMarks(
    const objects::PdfSourceDocument& source);

// 移除某頁（pageIndex < 0 代表全部頁面）的 /Redact 標記。
// 標記可逆的具體實作：把註解從 /Annots 拿掉，仍然是純附加的寫入。
[[nodiscard]] MarkWriteResult removeRedactionMarks(objects::IncrementalAppender& appender,
                                                   int pageIndex = -1);

}  // namespace alioth::engine::redaction
