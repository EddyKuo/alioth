#pragma once

// 從純文字建立 PDF（PRD-IO-012 的文字檔／剪貼簿路徑，WBS 15）。
//
// 斷行、分頁與邊距全部由 domain::create::layoutPlainText 決定，這一層只把
// 算好的每一行翻成 BT/Td/Tj。分成兩層的理由是斷行規則需要密集測試，而那
// 不該每次都產生一份完整的 PDF 才驗得到。
//
// 非 ASCII 明確失敗，不做任何替換。標準 14 字型走 WinAnsiEncoding，畫不出
// CJK；而字型內嵌的授權策略在 CLAUDE.md 仍是待決策項。靜默輸出的後果是
// 一整頁的空白或方框，使用者無從得知是內容沒進去還是字型沒裝——
// SDD §7 要求降級必須看得見，這裡連降級都談不上，只能失敗。

#include <cstddef>
#include <string>
#include <string_view>

#include "domain/document_source.h"
#include "engine/create/pdf_document_builder.h"

namespace alioth::engine::create {

struct TextImportResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
    std::size_t pageCount{0};
    std::size_t lineCount{0};

    // 非 ASCII 造成的失敗與「邊距算不出版心」是兩種完全不同的問題，
    // 呼叫端對前者要提示字型限制、對後者要提示調整選項。
    bool nonAscii{false};
};

// 把已算好的版面寫進 builder。回傳新增的頁數。
std::size_t appendTextPages(PdfDocumentBuilder& builder,
                            const domain::create::TextLayout& layout);

[[nodiscard]] TextImportResult createPdfFromPlainText(
    std::string_view text, const domain::create::TextImportOptions& options = {});

}  // namespace alioth::engine::create
