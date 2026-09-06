#pragma once

// 註解幾何的座標變換（WBS 12，PRD-PAGE-007 / 011 的隱藏需求）。
//
// 頁面內容被縮放或平移之後，註解不會自己跟著走：它們的幾何寫在註解字典裡，
// 與內容串流完全無關。漏掉這一步的症狀不是崩潰也不是錯誤訊息，而是
// 「頁面看起來完全正確，螢光筆卻留在原本的位置」——最難被回報、也最難被相信的一類缺陷。
//
// 要搬的鍵不只 /Rect。/Rect 只是外框，實際畫出來的形狀來自 /QuadPoints
// （螢光筆、底線、刪除線）、/InkList（手繪）、/Vertices（多邊形）、/L（直線）、
// /CL（引線）。只搬 /Rect 的結果是外框對了、標記本身還在原地。
//
// /AP 外觀串流刻意**不**動：ISO 32000-2 §12.5.5 的演算法會把 /AP 的 /BBox
// 經 /Matrix 之後的外框重新映射到 /Rect，因此改 /Rect 就等於同步縮放了外觀。
// 自己再去乘一次 /Matrix 會變成縮放兩次。這個取捨有一個已知代價：
// 旋轉時外觀本身不會轉，只有位置正確——見 page_merge.h 對 /Rotate 的說明。

#include <set>
#include <vector>

#include "domain/page_compose.h"
#include "engine/pageops/compose_document.h"

namespace alioth::engine::pageops {

// 頁面 /Annots 裡以間接參照登記的註解物件編號。
// 直接內嵌的註解字典（罕見但合法）會被跳過並回報，因為就地改它需要先搬成獨立物件。
[[nodiscard]] std::vector<int> pageAnnotationObjects(const PdfDocumentRewriter& document,
                                                     const PdfRef& page);

void setPageAnnotationObjects(PdfDocumentRewriter& document, const PdfRef& page,
                              const std::vector<int>& annotations);

// 套用矩陣到一個註解物件的所有幾何鍵。同時把 /P 指向 newPage（若給定）。
//
// visited 是必要的而不是最佳化：/Popup 指向的彈出視窗註解通常**同時**也列在
// 頁面的 /Annots 裡，沒有這個集合就會被平移兩次，便利貼會飛到頁面外。
[[nodiscard]] bool transformAnnotation(PdfDocumentRewriter& document, int annotationObject,
                                       const domain::compose::Matrix& matrix,
                                       const PdfRef* newPage, std::set<int>& visited);

}  // namespace alioth::engine::pageops
