#pragma once

// 無障礙檢查器測試語料（PRD-A11Y-003）。
//
// 刻意把多種缺陷疊在同一份文件裡而不是每個測試各開一份最小語料：
// 檢查器的價值恰好在於「同時看到全貌」，而且這樣才驗得出「一個缺陷的偵測
// 不會被另一個缺陷的偵測邏輯誤觸」（例如 Table 底下巢狀的 TR／TD 不該被
// 誤判成又一個缺替代文字的一般元素）。
//
// 復用 tests/a11y 的 assemblePdf：同一份組裝邏輯只寫一次，兩邊都靠它驗證
// 我們對檔案結構的理解一致。

#include "../a11y/tagged_pdf_fixture.h"

namespace alioth::test {

// 缺陷組合：
//   - Catalog 沒有 /Lang，也沒有 /Info /Title（文件層級兩項）
//   - 標題從 H1 直接跳到 H3（跳過 H2）
//   - Table 底下只有 TR／TD，沒有 TH
//   - 一個 Figure 沒有 /Alt（沿用既有的替代文字檢查）
//   - 一則 FreeText 註解 /C 與 /DA 都是白色，對比 1:1
inline QByteArray makeDefectivePdf() {
    std::vector<QByteArray> objects;
    // 1: Catalog —— 沒有 /Lang，也沒有 /ViewerPreferences
    objects.push_back(
        "<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 5 0 R /MarkInfo << /Marked true >> >>");
    // 2: Pages
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    // 3: Page —— 沒有 /Info，帶一則 FreeText 註解
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> /Annots [11 0 R] >>");
    // 4: Contents
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    // 5: StructTreeRoot
    objects.push_back("<< /Type /StructTreeRoot /K 6 0 R >>");
    // 6: Document
    objects.push_back(
        "<< /Type /StructElem /S /Document /P 5 0 R /K [7 0 R 8 0 R 9 0 R 10 0 R] >>");
    // 7: H1
    objects.push_back("<< /Type /StructElem /S /H1 /P 6 0 R /Pg 3 0 R /T (Intro) /K 0 >>");
    // 8: H3 —— 跳過 H2，是標題層級檢查要抓的那一個
    objects.push_back("<< /Type /StructElem /S /H3 /P 6 0 R /Pg 3 0 R /T (SubSub) /K 1 >>");
    // 9: Table —— 底下巢狀直接內嵌 TR/TD（不用另開物件），且刻意不放任何 /TH
    objects.push_back(
        "<< /Type /StructElem /S /Table /P 6 0 R /Pg 3 0 R "
        "/K [<< /Type /StructElem /S /TR /K [<< /Type /StructElem /S /TD /K 2 >>] >>] >>");
    // 10: Figure —— 沒有 /Alt
    objects.push_back("<< /Type /StructElem /S /Figure /P 6 0 R /Pg 3 0 R /K 3 >>");
    // 11: FreeText 註解 —— 文字與背景都是白色
    objects.push_back(
        "<< /Type /Annot /Subtype /FreeText /Rect [10 10 60 30] "
        "/C [1 1 1] /DA (1 1 1 rg /Helv 10 Tf) /Contents (test) >>");
    return assemblePdf(objects);
}

}  // namespace alioth::test
