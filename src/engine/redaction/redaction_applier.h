#pragma once

// 塗黑套用階段（PRD-ANN-032，WBS 11）。**不可逆**。
//
// 驗收標準是「移除後以文字擷取驗證原文不可還原」，因此這裡做的四件事缺一不可：
//
//   1. 內容串流裡落在區域內的顯示運算子與影像真的被拿掉（content_redactor）
//   2. 落在區域內的註解一併刪除——便利貼的 /Contents 不在內容串流裡，
//      只處理內容串流會讓註解裡的內容原封不動留著
//   3. 整份重寫而不是增量附加，讓被刪掉的位元組不再出現在檔案任何位置
//   4. 最後才在原位置畫上不透明的覆蓋矩形。它是給人看的，不是安全機制；
//      順序放在最後是為了避免有人把它當成第 1 步的替代品
//
// 加密文件明確拒絕（ADR-002 驗收條件 5）：字串與串流需要加密後才寫得回去，
// 半套的實作會產出「看起來成功、實際讀不出來」的檔案。

#include <string>

#include "domain/redaction.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::redaction {

struct ApplyStats {
    int removedStrings{0};
    int removedImages{0};
    int removedInlineImages{0};
    int removedAnnotations{0};
    int editedForms{0};
    int pagesTouched{0};
};

struct ApplyResult {
    bool ok{false};
    std::string diagnostic{};
    std::string bytes{};  // 完整的新檔（不是增量段）
    ApplyStats stats{};
};

// 依計畫套用塗黑。輸入是原檔位元組，輸出是全新的檔案位元組。
[[nodiscard]] ApplyResult applyRedactions(std::string sourceBytes,
                                          const domain::RedactionPlan& plan);

// 套用文件裡既有的 /Redact 標記，並在套用後把標記本身移除
// （標記留著會讓人以為還沒處理，而它此時已經沒有任何作用）。
[[nodiscard]] ApplyResult applyMarkedRedactions(std::string sourceBytes,
                                                domain::IrreversibleConsent consent);

}  // namespace alioth::engine::redaction
