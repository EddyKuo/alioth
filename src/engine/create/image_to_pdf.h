#pragma once

// 從影像建立 PDF（PRD-IO-009，WBS 15）。
//
// 三件事決定了這一檔的形狀：
//
// 1. JPEG 一律原樣嵌入。PDF 的 DCTDecode 濾鏡吃的就是 JPEG 位元流，解碼再
//    重編碼會同時損失畫質與時間，而掃描器與相機的輸出幾乎全是 JPEG——
//    那正是本需求最主要的輸入。原樣嵌入也讓「產出的影像資料與來源逐位元組
//    相同」變成可以直接驗證的性質。
// 2. 有 alpha 的影像必須產生 /SMask。PDF 的影像沒有 alpha 通道的概念，
//    把 RGBA 當成 RGB 寫進去會讓每四個取樣被當成一又三分之一個像素；
//    就算只丟掉 A 通道，透明處也會變成黑色而不是白色，因為那些像素的
//    RGB 本來就沒有定義。
// 3. 影像不解碼、不縮放。像素資料由呼叫端提供（平台層負責讀檔與解碼），
//    這一層只做座標換算與 PDF 物件組裝，因此可以在沒有任何影像庫的情況下
//    完整測試。
//
// 這一層不碰檔案系統：輸出是位元組，落檔屬於平台層。

#include <cstddef>
#include <string>
#include <vector>

#include "domain/document_source.h"
#include "engine/create/pdf_document_builder.h"

namespace alioth::engine::create {

struct ImageImportResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
    std::size_t pageCount{0};
    std::size_t smaskCount{0};  // 產生了幾個柔性遮罩，供呼叫端與測試核對

    // 每一頁的實際尺寸（點）。呼叫端要顯示「輸出為 A4」之類的摘要時需要，
    // 而重新推導一次換算等於把同一條規則寫兩份。
    std::vector<domain::create::ImagePlacement> placements;
};

// 把單一影像加成一頁。回傳頁面物件編號，0 代表失敗。
// 分開暴露是為了讓「影像 + 文字」之類的混合來源能共用同一段組裝邏輯。
int addImagePage(PdfDocumentBuilder& builder, const domain::create::SourceImage& image,
                 const domain::create::ImageImportOptions& options,
                 std::string* diagnostic = nullptr, bool* producedSmask = nullptr);

[[nodiscard]] ImageImportResult createPdfFromImages(
    const std::vector<domain::create::SourceImage>& images,
    const domain::create::ImageImportOptions& options = {});

}  // namespace alioth::engine::create
