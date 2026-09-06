#pragma once

// 註解寫入器（WBS 4.1–4.3 的 PDFium 端）。
//
// 這是本子系統唯一連結 PDFium 的檔案。外觀串流由 appearance_stream 產生，
// 這裡只負責建立註解物件、填齊字典鍵，並把產生好的 /AP /N 掛上去。
//
// 註解一律以獨立物件寫進 /Annots，不碰頁面內容串流——那是「不破壞原檔與
// 既有簽章」這個賣點的技術基礎，也是增量儲存能維持在 20 KB 以內的前提。

#include <cstddef>
#include <string>

#include "domain/annotation.h"
#include "engine/annotations/appearance_stream.h"

namespace alioth::engine::annotations {

// FPDF_PAGE 的不透明別名。引擎轉接層以外的程式碼不該知道它是什麼。
using PageHandle = void*;

struct WriteResult {
    bool ok{false};
    int index{-1};                 // 在 /Annots 中的索引
    bool appearanceWritten{false}; // /AP /N 是否成功寫入
    std::string diagnostic{};

    // 透明度與混合模式因 PDFium 的 /AP 介面限制而未進入外觀串流。
    //
    // FPDFAnnot_SetAP 只會建立 Form XObject 串流本身，不會建立 /Resources，
    // 所以外觀裡不能引用 /ExtGState。此時透明度改由註解字典的 /CA 承載
    // （符合規格，多數檢視器會套用），但螢光筆的 Multiply 混合會遺失。
    // 這是已知降級，必須讓上層看得到，不能安靜地吞掉。
    bool blendModeElided{false};
};

// 把一則註解寫進頁面。page 必須是 PDFium 的 FPDF_PAGE。
[[nodiscard]] WriteResult writeAnnotation(PageHandle page, const domain::Annotation& annotation,
                                          const AppearanceOptions& options = {});

}  // namespace alioth::engine::annotations
