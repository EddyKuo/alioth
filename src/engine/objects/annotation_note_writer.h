#pragma once

// 修改一則既有註解的註釋文字（PRD-ANN-004 的彈出視窗編輯）。
//
// 只動 /Contents 與 /M，不動幾何、不動 /AP。這對「便利貼」這類註解是正確的：
// 它的外觀是一個固定圖示，註釋文字只出現在彈出視窗裡，兩者本來就沒有關係。
//
// 但對外觀由文字決定的註解就不是——/FreeText 與 /Redact 的 /AP 串流裡畫的
// 就是 /Contents 本身。只改字典不重畫 /AP，Acrobat 會顯示舊文字（它信任 /AP），
// 而 macOS 預覽可能顯示新文字，同一份檔案在兩個檢視器上長得不一樣。
// 這裡因此**明確拒絕**那兩種子型，而不是預設它們也能用——留給呼叫端一個
// 看得見的錯誤，比留下一則自相矛盾的註解好。

#include <string>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct NoteEditResult {
    bool ok{false};
    std::string diagnostic{};
};

// contents 為 UTF-8；空字串代表移除 /Contents（等同清空註釋）。
// modifiedDate 為 PDF 日期字串（見 domain::toPdfDateString），空字串代表不動 /M。
[[nodiscard]] NoteEditResult setAnnotationContents(IncrementalAppender& appender,
                                                   int annotationObjectNumber,
                                                   const std::string& contents,
                                                   const std::string& modifiedDate);

}  // namespace alioth::engine::objects
