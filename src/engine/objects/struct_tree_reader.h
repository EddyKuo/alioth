#pragma once

// 讀取 /StructTreeRoot 的邏輯結構樹（ISO 32000-1 §14.7，PRD-A11Y-001 / PRD-A11Y-004）。
//
// 為什麼走物件層而不是 PDFium：PDFium 的 FPDF_StructTree 系列只給得出
// 元素型別與少數屬性，取不到 /Alt、/ActualText、/Lang，也無法區分
// 「這份文件沒有標籤」與「有標籤但我讀不出來」。而那個區分正是無障礙檢查
// 最重要的一項判定——把「未標籤」顯示成空面板，等於告訴使用者這份文件
// 沒問題，那是相反的結論。
//
// 因此這裡的回傳一律帶明確狀態：NoStructTree（確定沒有）、Malformed
// （有 /StructTreeRoot 但結構壞掉）、Ok。三者在 UI 上是三種不同的訊息，
// 不可合併。
//
// PDF 是不可信任輸入：/K 可以互相指涉造成無限遞迴，/Kids 可以有百萬個
// 節點。走訪一律迭代 + 節點上限 + 已訪集合，讀壞檔的後果是截斷並標記，
// 不是崩潰。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::objects {

enum class StructTreeStatus {
    Ok,
    NoStructTree,   // Catalog 沒有 /StructTreeRoot：這份文件不是標籤化 PDF
    Malformed,      // 有 /StructTreeRoot 但不是字典，或根本沒有任何可用的 /K
    Truncated,      // 讀到了，但節點數或深度超過上限，只回傳前一段
};

[[nodiscard]] const char* describe(StructTreeStatus status) noexcept;

// 結構元素。刻意是扁平的值型別 + 子節點向量，而不是指回原始 PdfObject 的
// 指標：面板的生命週期比 PdfSourceDocument 長（使用者關檔後面板還在），
// 保留指標會變成懸空存取。
struct StructNode {
    std::string type;         // /S 標準結構型別，例如 "P"、"H1"、"Figure"、"Table"
    std::string title;        // /T，作者自訂標題（可空）
    std::string altText;      // /Alt，替代文字（PRD-A11Y-004）
    std::string actualText;   // /ActualText，實際文字取代
    std::string language;     // /Lang
    std::string expansion;    // /E，縮寫展開
    std::int32_t pageIndex{-1};  // /Pg 對應的頁序；找不到為 -1
    int objectNumber{0};      // 來源物件編號；0 表示直接物件（無法回寫）
    // 這個元素直接擁有的 MCID（/K 陣列裡與子元素混放的裸整數）。
    // 不含子結構元素自己的 MCID——那些算在子元素頭上。
    //
    // 用途是 PRD-A11Y-002 的閱讀順序面板：MCID 是內容串流實際輸出順序的忠實記錄
    // （標記內容運算子 BDC/EMC 依內容產生順序遞增），因此「依 MCID 排序」給出的是
    // 一條與結構順序完全獨立、且不需要額外解析內容串流位置就能取得的比對基準。
    // 這不是幾何位置（兩者不能等價），但兩者不一致時暴露的問題高度重疊：
    // 結構順序被人工重排、但內容其實還是照原本繪製順序輸出，正是最常見的缺陷成因。
    std::vector<std::int32_t> mcids;
    std::vector<StructNode> children;

    // 遞迴找出這個節點與其所有子孫擁有的最小 MCID；找不到回傳 -1。
    // 「最小」而不是「第一個」：/K 陣列內子元素與裸 MCID 的先後順序在規格上
    // 沒有強制意義，但同一份文件裡 MCID 的指派幾乎必然單調遞增，取最小值
    // 等同取這個節點在內容串流裡「最早出現」的位置。
    [[nodiscard]] std::int32_t minMcid() const noexcept;

    // 需要替代文字卻沒有的元素（PRD-A11Y-004 的檢查依據）。
    // /Figure 與 /Formula 沒有 /Alt 也沒有 /ActualText 時，螢幕閱讀器
    // 只會念出空白——使用者不會知道那裡有東西，比明顯的錯誤更糟。
    [[nodiscard]] bool needsAlternateText() const noexcept;
    [[nodiscard]] bool hasAlternateText() const noexcept {
        return !altText.empty() || !actualText.empty();
    }
};

struct StructTree {
    StructTreeStatus status{StructTreeStatus::NoStructTree};
    std::vector<StructNode> roots;
    std::size_t nodeCount{0};
    // /MarkInfo /Marked。有 /StructTreeRoot 但 /Marked 為 false 的文件是
    // 半成品：結構樹存在卻沒有標記內容可對應，輔助技術讀出來的順序不保證。
    bool markedContent{false};
    std::string diagnostic;
};

// 走訪上限。挑這兩個數字的理由：真實的標籤化文件（600 頁技術手冊）約
// 十萬個結構元素，深度極少超過 40；超過就幾乎確定是惡意或損毀的檔案。
inline constexpr std::size_t kMaxStructNodes = 200000;
inline constexpr int kMaxStructDepth = 64;

[[nodiscard]] StructTree readStructTree(const PdfSourceDocument& source);

// 攤平成前序清單，供無障礙檢查與測試逐項比對用。
[[nodiscard]] std::vector<const StructNode*> flatten(const StructTree& tree);

}  // namespace alioth::engine::objects
