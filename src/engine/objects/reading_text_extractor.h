#pragma once

// 朗讀（Read Out Loud，PRD-A11Y-006）的文字擷取。
//
// 這裡刻意只做「取出可以誠實朗讀的文字」，不做語音合成——那是平台層的事
// （見 platform/speech_synthesis.h，Windows 走 SAPI）。
//
// 誠實的邊界：結構樹（struct_tree_reader）只記錄 /Alt、/ActualText、/T 這幾個
// 明確的文字欄位，**不記錄一般段落（/P、/Span）真正的可見文字**——那些文字活在
// 內容串流裡，要拿到它得把 MCID 對回頁面上的文字物件，這是一套獨立的擷取子系統
// （類似 engine/text 但反向：從 MCID 找文字而不是從座標找文字），不在本模組範圍。
//
// 因此本模組的朗讀來源只有三種，且明確不假裝涵蓋更多：
//   1. /ActualText：規格明訂的「用這段文字取代」，任何有它的節點都用它朗讀。
//   2. /Alt：Figure／Formula／Form／Link 的替代文字。
//   3. /T：標題型節點（H1~H6、Table、Figure 等）在沒有 ActualText 時的次選——
//      朗讀一個標題的標題文字，好過完全跳過整個章節。
// 其餘沒有這三者的節點（絕大多數 /P、/Span）一律跳過並計入
// ReadingExtractionResult::skippedNoTextCount，讓呼叫端能誠實地告訴使用者
// 「這頁有些內容目前朗讀不出來」，而不是安靜漏讀。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/objects/struct_tree_reader.h"

namespace alioth::engine::objects {

struct ReadingUtterance {
    std::string text;           // 已分句的一句
    std::string sourceType;     // 來源節點的 /S，供除錯與（未來）朗讀時的定位高亮
};

struct ReadingExtractionResult {
    std::int32_t pageIndex{-1};
    std::vector<ReadingUtterance> utterances;

    // 有結構、但三種來源都沒有，因此略過的節點數（多半是純 /P、/Span）。
    int skippedNoTextCount{0};
    // Figure／Formula／Form／Link 缺替代文字而略過的節點數（PRD-A11Y-004 的缺陷，
    // 在朗讀情境下的直接後果就是使用者完全不知道那裡有內容）。
    int skippedMissingAltCount{0};
};

// 把一段已擷取的文字切成句子。中英文終止符號都認：。！？.!? 與換行。
// 標點留在句尾（螢幕閱讀器的停頓由標點決定，切掉會讓語氣變平）。
// 純函數，不含任何語言判斷之外的邏輯，方便獨立測試。
[[nodiscard]] std::vector<std::string> splitIntoSentences(const std::string& text);

// 擷取指定頁面的朗讀內容，已依結構順序排列並分句。
[[nodiscard]] ReadingExtractionResult extractReadingText(const StructTree& tree,
                                                         std::int32_t pageIndex);

}  // namespace alioth::engine::objects
