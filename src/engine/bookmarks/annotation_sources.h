#pragma once

// 由高亮註解產生書籤的資料來源（WBS 9，PRD-BM-014）。
//
// 只做「把 /Annots 裡的高亮撈出來」這一件事，組樹交給
// domain::bookmarks::bookmarksFromHighlights。分開的理由是排序與標題截斷的
// 規則完全不需要 PDF，而撈註解完全不需要判斷閱讀順序。
//
// 走物件層而不是 PDFium：這個 target 刻意不連結 PDFium（見 CMakeLists），
// 而高亮的 /Rect 與 /Contents 用物件層讀出來就夠了。
//
// **已知限制**：高亮註解不一定帶 /Contents。PDF 的高亮只記錄 /QuadPoints，
// 被標起來的那段文字要靠文字層擷取才拿得到，而文字層屬於 alioth_text
// （它有自己的 PDFium 文件把手與執行緒）。因此 /Contents 是空的時候，
// 這裡回傳的 text 也是空的，並在 needsTextLookup 標記出來，由呼叫端決定
// 要不要再跑一次文字擷取補上——靜默給一個空標題會讓使用者拿到一整排空書籤。

#include <cstdint>
#include <vector>

#include "domain/bookmark_ops.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::bookmarks {

struct HighlightRecord {
    domain::bookmarks::HighlightSource source{};
    bool needsTextLookup{false};  // /Contents 是空的，標題要靠文字層補
};

// 依頁序、同頁由上而下列出所有高亮註解。
[[nodiscard]] std::vector<HighlightRecord> collectHighlights(
    const objects::PdfSourceDocument& source);

// 只取 source 欄位的便利版本，直接餵給 bookmarksFromHighlights。
[[nodiscard]] std::vector<domain::bookmarks::HighlightSource> highlightSources(
    const std::vector<HighlightRecord>& records, bool includeUntitled = false);

}  // namespace alioth::engine::bookmarks
