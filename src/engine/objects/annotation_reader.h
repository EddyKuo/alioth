#pragma once

// 從既有 PDF（或 FDF）的註解字典讀回領域模型。
//
// 這是 annotation_object_writer 的反向。存在的理由是「匯出註解」：使用者要把
// 挑出來的幾則意見交給別人，就必須先把檔案裡的註解**完整**讀出來——
// domain::AnnotationSummary 只夠畫列表（頁碼、作者、內容、外接矩形），
// 匯出需要 /QuadPoints、/InkList、/Vertices 這些真正決定形狀的鍵。
//
// 走物件層而不是 PDFium：PDFium 的 FPDFAnnot_* 讀得到的鍵是它決定的子集，
// 而這裡需要的是字典本身。這也讓匯出路徑與寫入路徑用同一組欄位定義，
// 「寫得出來卻讀不回來」這種不對稱在往返測試裡會直接現形。
//
// 不可信任輸入：任何鍵的型別不符一律當成「沒有這個鍵」，不硬轉；
// 認不得的 /Subtype 回傳 std::nullopt 由呼叫端決定要跳過還是報錯。

#include <optional>
#include <vector>

#include "domain/annotation.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_object.h"

namespace alioth::engine::objects {

// 支援的子型與 XFDF／FDF 相同：文字標記四種、Square、Circle、Line、Ink、
// Text、Caret、FreeText、Polygon、PolyLine。其餘（Widget、Popup、Stamp…）
// 回傳 std::nullopt。
[[nodiscard]] std::optional<domain::Annotation> readAnnotation(const PdfDictionary& dict);

struct PageAnnotation {
    int pageIndex{0};
    int indexOnPage{0};
    domain::Annotation annotation{};
};

// 走遍全部頁面的 /Annots。不支援的子型直接略過，不會在回傳值裡佔位——
// 呼叫端拿到的每一項都是可以原樣寫回去的。
[[nodiscard]] std::vector<PageAnnotation> readAllAnnotations(IncrementalAppender& appender);

}  // namespace alioth::engine::objects
