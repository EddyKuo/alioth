#pragma once

// 矩形（區域）選取複製為 TSV（PRD-TXT-003）。純 C++，與 PDFium 及 Qt 無關——
// 輸入只是已擷取好的 PageTextLayer，這件事本質上是幾何分群，不需要再問 PDFium。
//
// PDF 沒有表格結構：內容串流只有一串帶座標的字元，「這幾個字屬於同一列/同一欄」
// 是版面上的視覺事實，不是檔案裡的資料。因此這裡完全不假設任何 /StructTree 或
// 表格標記，只靠字元外框的座標做兩層分群：
//
//   第一層（列）：與 text_extractor.cpp 的斷行判定同一套「垂直重疊比例」邏輯
//                （見 kSameLineOverlapRatio），但這裡重新對選取範圍內的字元
//                做一次，而不是沿用 PageTextLayer 既有的 lineIndex——原始文字
//                流的順序常常不是視覺順序（例如表格常見的「先印完一欄才印下一
//                欄」），若照抄 lineIndex 會把不同欄的字錯誤地黏成一行。
//
//   第二層（欄）：先在每一列內部把相鄰字元合併成「詞」（gap 小於字高的一個比例
//                視為同一詞），再把所有列的詞的水平範圍投影到同一條軸上做聯集。
//                投影出的每一段連續「有墨水」區間就是一欄——這是所謂的
//                「空白欄分隔」（whitespace projection）演算法，pdfplumber 等
//                工具的 stream 模式用的正是同一個想法。選它而不是「以詞的置中點
//                分群」的原因是它能正確處理數字靠右對齊：欄的邊界由「所有列在
//                這段水平範圍內都沒有墨水」決定，跟個別詞的對齊方式無關。
//
// 這個演算法有一個結構性、無法迴避的限制，必須明說：**跨欄合併儲存格**。
// 若任一列有一格文字橫跨兩欄的水平範圍，投影會把那兩欄在全表範圍內融合成一欄
// ——不只是那一列，是整張表。原因是欄邊界由「全部列」的聯集決定，沒有表格網格線
// 或儲存格級中繼資料，無法只讓某一列「跳過」一次融合。偵測到這個情形時
// ExtractedTable::ambiguous 會設為 true，讓呼叫端可以在 UI 上提示使用者
// 「欄位對齊可能不準確，建議手動核對」，但仍然回傳盡可能完整的文字內容
// （融合欄位內的多個詞以空白接起來，不會遺漏文字）。

#include <cstdint>
#include <string>
#include <vector>

#include "geometry.h"
#include "text_layer.h"

namespace alioth::domain {

struct ExtractedTable {
    // rows[i][j] 是第 i 列、第 j 欄的文字（UTF-8）。所有列的長度都等於
    // columnBoundaries.size()，缺格的儲存格是空字串而不是被省略——
    // 省略會讓下游（貼到 Excel）的欄位對不上。
    std::vector<std::vector<std::string>> rows;

    // 偵測到的欄左邊界（頁面座標，點），由左到右排序。數量即欄數。
    std::vector<double> columnBoundaries;

    // 為真代表偵測到至少一次欄位融合（見檔頭「跨欄合併儲存格」的說明）。
    // 這不是錯誤，是這個演算法在沒有表格結構資訊時的已知上限。
    bool ambiguous{false};
};

// 從一頁文字層裡，把落在 area 內（以字元外框與 area 相交判定）的字元
// 分群成列與欄。area 使用頁面座標（點，原點左下、Y 向上），與 RectF 一致。
[[nodiscard]] ExtractedTable extractTable(const PageTextLayer& layer, const RectF& area);

// 把已分群的表格序列化成 TSV：欄以 \t 分隔、列以 \n 結尾。
// 儲存格文字裡若含 Tab 或換行（理論上不該出現在單一儲存格內，但字型或掃描雜訊
// 可能製造出這種字元），一律摺成單一空白——TSV 沒有像 CSV 那樣普遍受支援的
// 欄位跳脫慣例，貼到 Excel 時保留原始 Tab 只會把一格拆成兩格。
[[nodiscard]] std::string tableToTsv(const ExtractedTable& table);

// 便捷入口：分群後直接回傳 TSV。多數呼叫端（複製到剪貼簿）只需要這個結果。
[[nodiscard]] std::string extractTsv(const PageTextLayer& layer, const RectF& area);

}  // namespace alioth::domain
