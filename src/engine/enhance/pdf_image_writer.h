#pragma once

// 把編碼後的影像寫成 PDF 影像 XObject（WBS 14，PRD-ENH-001 / 003 / 004 共用）。
//
// 三個功能都要做同一件事：一份影像位元組 → 一個 /Subtype /Image 的串流物件，
// 必要時加上 /SMask。抽出來共用的理由不只是省程式碼，而是 /SMask 的尺寸與
// 主影像必須一致這條規則只要有一處漏掉，透明度就會整片錯位，
// 而那個症狀在縮圖上看不出來。
//
// 寫入走 IncrementalAppender（純附加），因此既有的數位簽章仍然顯示為
// 「有效，簽章後有變更」而不是「無效」（SDD §1.3）。

#include <string>

#include "domain/geometry.h"
#include "engine/enhance/image_codec.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::enhance {

struct ImageWriteResult {
    bool ok{false};
    std::string diagnostic{};
    int imageObject{0};
    int softMaskObject{0};  // 0 代表來源不透明
};

// 建立影像 XObject（含 /SMask）。回傳物件編號，掛到哪個頁面由呼叫端決定。
[[nodiscard]] ImageWriteResult writeImageObject(objects::IncrementalAppender& appender,
                                                const EncodedImage& image);

// 不含間接參照的影像串流本體，供不走增量附加的路徑（例如全檔重寫）使用。
[[nodiscard]] objects::PdfObject makeImageStream(const EncodedImage& image,
                                                 int softMaskObjectNumber = 0);

// 繪製影像的內容串流片段：把單位正方形映到 rect。
//
// 影像空間的單位正方形原點在左下、Y 向上，與 PDF 使用者空間一致，
// 因此這裡不需要翻轉——但影像資料本身是由上而下排列的，那個翻轉在
// PDF 的影像模型裡已經內建（第一列取樣對應單位正方形的頂邊）。
// 兩邊都翻是這個地方最常見的錯誤，症狀是圖上下顛倒。
[[nodiscard]] std::string drawImageContent(const std::string& resourceName,
                                           const domain::RectF& rect);

// 純色矩形的內容串流片段。
[[nodiscard]] std::string fillRectContent(const domain::ColorRgb& color,
                                          const domain::RectF& rect);

// 頁面的可見矩形：/CropBox 優先，缺少時退回 /MediaBox。
// 兩者都可能從 /Pages 繼承，因此不能只看頁面自己的字典。
[[nodiscard]] bool pageBox(const objects::PdfSourceDocument& source, const objects::PdfRef& page,
                           domain::RectF& out);

// 把一個內容串流插到頁面既有內容的**前面**。
//
// 「背景在下、浮水印在上」的差別就只是這個函式與 appendPageContent 的差別。
// /Contents 有四種形態（不存在、直接陣列、指向陣列的參照、指向單一串流的參照），
// 少處理任何一種的後果都不是崩潰，而是背景沒出現或原有內容消失。
[[nodiscard]] bool prependPageContent(objects::IncrementalAppender& appender,
                                      const objects::PdfRef& page, int contentObject);

// 以單一內容串流取代頁面的全部內容（點陣化）。
// 原本的內容串流物件仍然留在檔案裡（純附加的必然結果），只是不再被引用。
[[nodiscard]] bool replacePageContent(objects::IncrementalAppender& appender,
                                      const objects::PdfRef& page, int contentObject);

}  // namespace alioth::engine::enhance
