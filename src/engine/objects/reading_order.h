#pragma once

// 閱讀順序比對（PRD-A11Y-002）。
//
// 螢幕閱讀器實際念出的順序，來自 /StructTreeRoot 的結構順序——這是本模組唯一
// 承認的「權威順序」。但單獨顯示結構順序看不出問題：結構順序本身沒有錯誤可言，
// 錯的是它與「內容實際被畫出來的順序」不一致時，使用者聽到的內容會跳來跳去。
//
// 因此本模組額外算出第二條獨立的順序軸——依 MCID（標記內容識別碼）排序。
// MCID 是內容串流輸出時依序遞增指派的（BDC/EMC 巢狀計數器），不需要額外解析
// 內容串流位置就能從結構樹本身取得（見 struct_tree_reader.h 的 StructNode::mcids）。
// 這不是幾何位置：兩個逐字重疊、視覺上不相鄰的元素可能有相鄰的 MCID。
// 但兩者不一致時暴露的缺陷高度重疊（結構被人工重排、內容繪製順序沒有跟著變），
// 且這條軸不需要新增任何 PDFium 呼叫或內容串流座標追蹤，是在不擴大引擎轉接層
// 相依範圍的前提下可以誠實取得的比對基準。
//
// 明確不做的事：真正的「幾何／視覺順序」（依頁面座標由上到下、由左到右）。
// 一般段落（/P、/Span）沒有 BBox 屬性，要拿到它們的視覺位置得逐字元追蹤內容串流
// 座標並與 MCID 對應——那是一套獨立的文字位置擷取子系統，不是這個模組的範圍。
// 呼叫端（Order 面板／無障礙檢查器）必須把這個限制對使用者說清楚，不能把
// 「MCID 順序」包裝成「視覺順序」。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/objects/struct_tree_reader.h"

namespace alioth::engine::objects {

struct PageOrderItem {
    std::string type;   // /S 標準結構型別
    std::string title;  // /T，可空
    std::string altOrActualText;  // /Alt 或 /ActualText，可空
    int depth{0};        // 相對於本頁項目清單根層的巢狀深度，供 UI 縮排
    int structureRank{-1};  // 在結構順序中的序（0-based，即 items 的索引）
    int contentRank{-1};    // 依 MCID 由小到大排序後的序；-1 表示這個節點沒有可用的 MCID
};

struct PageReadingOrder {
    std::int32_t pageIndex{-1};
    // 一律依結構順序（權威順序）排列。
    std::vector<PageOrderItem> items;
    // items 的索引：這個項目在「有已知內容順序的子集合」裡，相對名次與結構名次不同。
    // 只在有 MCID 的項目之間比較——沒有 MCID 的項目無法公平比較，不強行湊數字。
    std::vector<int> mismatchIndices;
    // 有結構順序、但沒有任何可判斷內容順序（無 MCID）的項目數。
    // 面板必須顯示這個數字，否則「沒有標紅」會被誤讀成「順序一致」，
    // 而真相可能只是「這裡量不到」。
    int unknownContentOrderCount{0};
};

// 計算指定頁面的結構順序與內容順序比對。tree 必須先由 readStructTree 讀出。
// 只收錄 pageIndex 完全相符的節點（沒有 /Pg 或 /Pg 指到別頁的節點不計入本頁）。
[[nodiscard]] PageReadingOrder computePageReadingOrder(const StructTree& tree,
                                                       std::int32_t pageIndex);

}  // namespace alioth::engine::objects
