#pragma once

// 內容串流的塗黑編輯（PRD-ANN-032 的核心，WBS 11）。
//
// 「蓋一個黑色矩形上去」是 Redaction 最常見也最嚴重的事故：底下的文字仍然
// 可以被複製、被搜尋、被文字擷取讀出來。因此這一層做的是**把顯示運算子從
// 內容串流裡拿掉**，黑色矩形是另一件事（給人看的），兩者不可互相取代。
//
// 判斷「落在區域內」需要重放內容串流的圖形狀態與文字狀態：CTM、Tm、字型大小、
// 字距、字寬。這等於實作一個不畫圖的解譯器，而它的每一個估算誤差都有方向性：
// 外框估小 → 邊緣的字留在檔案裡（外洩）；估大 → 多刪幾個字（可惜但安全）。
// 本檔所有不確定的地方一律往「估大」倒，這個取捨在各處都以註解標明。
//
// 移除後不是直接刪掉位元組，而是換成等寬的 TJ 位移調整量：顯示運算子會改變
// 文字矩陣，整段刪掉會讓同一行後面的字往前跑，看起來像是「塗黑順便打亂了排版」。
// 用 TJ 數字補回位移可以讓剩下的字留在原位，而且它不帶任何字碼。

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "domain/geometry.h"
#include "domain/redaction.h"
#include "engine/objects/pdf_object.h"
#include "engine/redaction/pdf_document_rewriter.h"

namespace alioth::engine::redaction {

// [a b c d e f]，與 PDF 的 cm / Tm 運算元順序一致。
using Matrix = std::array<double, 6>;

[[nodiscard]] constexpr Matrix identityMatrix() noexcept { return {1, 0, 0, 1, 0, 0}; }

// a 先套用、b 後套用。順序寫反的症狀是圖形位置整體錯位，而不是崩潰。
[[nodiscard]] Matrix multiply(const Matrix& a, const Matrix& b) noexcept;
[[nodiscard]] domain::PointF applyMatrix(const Matrix& m, double x, double y) noexcept;

// 單位正方形（影像與 Form XObject 的 /BBox 都以此為基礎）經矩陣後的外框。
[[nodiscard]] domain::RectF transformedBounds(const Matrix& m, const domain::RectF& box) noexcept;

struct ContentRedactionStats {
    int removedStrings{0};        // 被移除的顯示字串數（Tj / TJ 元素 / ' / "）
    int removedImages{0};         // 被移除的 Do 影像
    int removedInlineImages{0};   // 被移除的 BI…EI
    int editedForms{0};           // 被遞迴編輯的 Form XObject
};

struct ContentRedactionResult {
    bool ok{false};
    std::string diagnostic{};
    std::string content{};                          // 編輯後的內容串流（未壓縮）
    ContentRedactionStats stats{};
    std::vector<std::string> removedXObjectNames{}; // 呼叫端要從資源字典移除的名稱
};

struct ContentRedactionRequest {
    std::vector<domain::RectF> areas{};    // 頁面座標（點，原點左下）
    domain::PartialOverlapPolicy textPolicy{domain::PartialOverlapPolicy::RemoveWholeString};
    domain::ImageOverlapPolicy imagePolicy{domain::ImageOverlapPolicy::RemoveWholeImage};
};

// 對一段內容串流執行塗黑。
//
// resources 是該串流適用的資源字典（頁面的 /Resources，必要時已解繼承）。
// baseCtm 是「內容座標 → 頁面座標」的矩陣；頁面內容是單位矩陣，
// Form XObject 遞迴時則帶著累積下來的矩陣。
//
// 遇到無法安全處理的情況（串流濾鏡解不開、要編輯的 Form XObject 被多處共用）
// 一律回傳 ok = false 並說明原因。靜默略過等於留下沒被塗黑的內容，
// 那是這個工作包唯一不能犯的錯。
[[nodiscard]] ContentRedactionResult redactContentStream(PdfDocumentRewriter& document,
                                                         const std::string& content,
                                                         const objects::PdfObject& resources,
                                                         const Matrix& baseCtm,
                                                         const ContentRedactionRequest& request,
                                                         int depth = 0);

}  // namespace alioth::engine::redaction
