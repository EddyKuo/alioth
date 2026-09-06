#pragma once

// 書籤樹的讀取（WBS 9）。
//
// 與 PdfiumEngine::outline() 的分工：那一條是給書籤面板用的，只需要標題與頁碼，
// 走 PDFium 的 FPDFBookmark_* API。這一條是給**批次操作**用的，需要縮放類型、
// 命名目標、顏色與樣式，而那些 PDFium 一個都給不出來——FPDFBookmark_GetDest
// 只回傳頁面索引，且完全不看 /A GoTo 形態的目標。
//
// 走訪必須抗畸形輸入：/Next 與 /First 都可以指回已經走過的節點，遞迴實作會
// 直接爆堆疊或無限迴圈。這裡用已訪問集合加深度上限，遇到迴圈就在該處截斷，
// 而不是整棵放棄——書籤壞掉的檔案仍然要能讓使用者修。

#include <cstddef>
#include <string>

#include "domain/bookmark_ops.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::bookmarks {

struct OutlineReadResult {
    bool ok{false};
    std::string diagnostic;
    domain::bookmarks::BookmarkTree tree;
    std::size_t itemCount{0};

    // 走訪時遇到重複造訪或深度超限而截斷的次數。非零代表原檔的書籤鏈有問題，
    // 值得在 UI 上提示而不是靜默修好——使用者存檔後那些節點就真的不見了。
    std::size_t truncated{0};
};

[[nodiscard]] OutlineReadResult readOutline(const objects::PdfSourceDocument& source);

}  // namespace alioth::engine::bookmarks
