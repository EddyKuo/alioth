#pragma once

// 圖層攤平為基礎內容（PRD-VIEW-014，R1 降級的還款計畫，見 ADR-003）。
//
// 背景：預編譯 PDFium 沒有任何 OCG 執行期可見性 setter（domain/ocg.h、
// exceptions/EXC_20260906_RD_SA_ocg_render_gap.md）。唯一能讓「使用者勾掉的
// 圖層真的從畫面上消失」的方式，是不倚賴 PDFium 的可見性控制，而是照著目前
// 面板上的勾選狀態（domain::OcgTree），把落在被關閉圖層裡的內容操作子從
// 內容串流裡整段拿掉，交出一份新檔案。之後不管用什麼檢視器開，隱藏圖層
// 都不會被畫出來——因為它已經不在檔案裡了。
//
// **不可逆**：被拿掉的內容操作子無法從輸出檔案還原。與 engine/redaction 及
// engine/objects/annotation_flattener 同一套慣例，入口函式要求呼叫端提供
// domain::IrreversibleConsent，讓「使用者已明確同意攤平」在原始碼裡留下一個
// 看得到的字面痕跡，而不是一個布林參數就悄悄跳過確認。
//
// 判準（ISO 32000-1 §8.11.4.3 標記內容；§8.11.2 起 OCG／OCMD）：
//
//   - BDC／EMC 巢狀配對：BDC、BMC 都會推入一層，只有 EMC 會彈出；
//     DP、MP 是點標記內容，不參與巢狀計數。配對不平衡（多餘的 EMC，或
//     串流結束時堆疊未清空）視為輸入損毀，明確失敗，不產生結構壞掉的頁面。
//   - 只有 tag 為 /OC 的 BDC 才可能改變可見性；其餘 tag（/Span、/Artifact…）
//     一律繼承外層的可見性，不自行判斷。
//   - /OC 的第二個運算元必須是名稱，經頁面 /Resources /Properties 解析成
//     間接參照，才可能找到目標字典——不合規的內嵌字典運算元、或解析不到
//     資源的情況，一律保守視為可見（寧可多顯示，攤平不可逆，錯誤的方向
//     只能是「留下不該留的」而不是「刪掉不該刪的」）。
//   - 目標字典可能是 OCG（直接查 tree 目前的可見性）或 OCMD
//     （/OCGs 列出的成員 + /P 決定的 AnyOn/AllOn/AnyOff/AllOff 政策，
//     見 domain::resolveOcmdPolicy）。/P 缺漏時依規範預設 AnyOn。
//   - OCMD 帶 /VE（可見性運算式）目前不支援：不嘗試求值，一律保守視為
//     可見，並在 FlattenResult 標記 sawUnsupportedVisibilityExpression
//     讓呼叫端把這個降級明確告知使用者（IL-4，不得靜默吞噬）。
//   - 影像與 Form XObject 若自己的字典上有 /OC，其 Do 呼叫本身依同一套
//     規則判斷是否要整段拿掉；Form XObject 內部若還有自己的標記內容，
//     會遞迴攤平（深度上限見 .cpp）。
//
// 與 engine/redaction 的塗黑不同：這裡的可見性判斷只依賴文件全域的 OCG
// 狀態，不依賴呼叫頁面的幾何或身份，因此被多個頁面共用的 Form XObject
// 可以直接就地改寫，不需要 redaction 那種「被多處共用就明確失敗」的保護
// ——攤平結果對每個呼叫端都一樣。但頁面內容一律另外產生新物件並改指
// /Contents，不就地覆寫原內容串流物件，這樣即使兩個頁面原本共用同一個
// 內容串流物件，也不會因為只改其中一邊而讓另一邊維持未攤平的原文。
//
// 刻意不連結 PDFium：走 ADR-002 的物件層通道（Alioth::objects），並重用
// engine/redaction 已經有的全檔重寫器（PdfDocumentRewriter）——它本來就是
// 通用的「可達性標記—清除」全檔輸出工具，不是塗黑專屬的邏輯，這裡只是
// 借用而不重寫一份。呼叫端因此會透過 Alioth::layers 間接連到
// Alioth::redaction（見 engine/layers/CMakeLists.txt 的說明），但攤平本身
// 沒有任何一行呼叫 PDFium API。

#include <string>

#include "domain/ocg.h"
#include "domain/redaction.h"

namespace alioth::engine::layers {

struct FlattenStats {
    int removedMarkedContentSpans{0};  // 被整段拿掉的 BDC...EMC（tag=/OC 且判定隱藏）
    int removedXObjectDraws{0};        // 因目標 XObject 自己的 /OC 判定隱藏而被拿掉的 Do
    int removedAnnotations{0};         // 因 /OC 判定隱藏而被移除的註解
    int editedForms{0};                // 被遞迴改寫的 Form XObject 數（去重後）
    int pagesTouched{0};
};

struct FlattenResult {
    bool ok{false};
    std::string diagnostic{};
    std::string bytes{};  // 完整的新檔（不是增量段）
    FlattenStats stats{};
    // OCMD 帶 /VE：目前不支援求值，保守視為可見。至少出現一次即為 true，
    // 呼叫端應該把這個降級明確顯示給使用者，而不是讓他們以為攤平是完整的。
    bool sawUnsupportedVisibilityExpression{false};
};

// 依 tree 目前的可見性狀態攤平圖層。tree 通常就是使用者在圖層面板上
// 勾選/取消勾選後的 domain::OcgTree（見 domain::setLayerVisible）；
// 攤平只讀取它的可見性，不會回寫。
//
// tree.present 為 false（文件本來就沒有 /OCProperties）時直接回傳失敗，
// 呼叫端不應該對沒有圖層的文件呼叫這個函式——那不是攤平的正常輸入。
[[nodiscard]] FlattenResult flattenLayers(std::string sourceBytes, const domain::OcgTree& tree,
                                          domain::IrreversibleConsent consent);

}  // namespace alioth::engine::layers
