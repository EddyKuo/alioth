#pragma once

// 儲存路徑的測試支援。不供產品程式碼使用。
//
// 為什麼需要這個：驗證「增量儲存只追加變更物件」必須先讓文件真的有變更，
// 而製造變更的正規途徑（註解 WBS 4.x、頁面管理 WBS 5.6）都不在本工作包內，
// 也不該為了測試而被本工作包搶先實作。這裡提供的是能讓 PDFium 把物件標記為
// 已變更的最小動作，僅此而已；它不是產品功能，也不會出現在任何 UI 路徑上。

#include "engine/save/incremental_saver.h"

namespace alioth::engine::save::support {

// 旋轉指定頁面，藉此在文件中製造一筆真實的物件變更。
bool rotatePage(DocumentHandle document, int pageIndex, int quarterTurns);

}  // namespace alioth::engine::save::support
