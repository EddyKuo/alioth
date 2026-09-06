#pragma once

// 無障礙檢查器（PRD-A11Y-003）。
//
// 這裡的每一項發現都要能被使用者拿去直接動手：在哪一頁、哪個元素、
// 為什麼算缺陷、怎麼修。做不到這四件事的判定不放進 findings，
// 而是放進 coverage 明白告訴使用者「這項沒檢查、原因是什麼」——
// 一份看起來全綠但其實漏查一半的報告，比不做檢查更危險：
// 使用者會把「沒有報告」當成「沒有問題」而放行文件。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/objects/pdf_source_document.h"
#include "engine/objects/struct_tree_reader.h"

namespace alioth::engine::objects {

enum class A11yCheckId {
    DocumentTitle,       // /ViewerPreferences /DisplayDocTitle 與 /Info /Title
    DocumentLanguage,    // Catalog /Lang
    TagStructure,        // /StructTreeRoot 是否存在且可用
    ImageAltText,        // Figure/Formula/Form/Link 缺 /Alt 或 /ActualText
    TableHeaders,        // /Table 底下沒有任何 /TH
    HeadingHierarchy,     // 標題層級跳級（例如 H1 之後直接 H3）
    ReadingOrder,        // 結構順序與內容（MCID）順序不一致
    Contrast,            // 對比不足（僅涵蓋量得到的部分，見下方 coverage 說明）
};

[[nodiscard]] const char* describe(A11yCheckId id) noexcept;

struct A11yFinding {
    A11yCheckId check{A11yCheckId::TagStructure};
    std::int32_t pageIndex{-1};  // -1 表示文件層級（非特定頁）
    std::string element;         // 哪個元素：型別＋物件編號或其他可定位描述
    std::string reason;          // 為什麼是缺陷
    std::string remedy;          // 怎麼修
};

struct A11yCoverageEntry {
    A11yCheckId check{A11yCheckId::TagStructure};
    bool supported{true};   // false 表示這項檢查在目前實作下做不到
    std::string note;       // supported 為 false 時，說明原因；為 true 時可放涵蓋範圍限制
};

struct A11yReport {
    std::vector<A11yFinding> findings;
    // 涵蓋清單固定包含 A11yCheckId 的每一項，不論 supported 與否——
    // 這樣「檢查通過」與「這項根本沒有跑」在報告上永遠可以分辨。
    std::vector<A11yCoverageEntry> coverage;

    // 完全沒有對應 A11yCheckId、因此連「有跑但範圍窄」都稱不上的項目——
    // 例如「顏色是否為唯一的語意辨識方式」（WCAG 1.4.1）、表單欄位標籤關聯、
    // 多媒體字幕。列出來不是自我否定，是不讓使用者誤以為報告已經涵蓋一切。
    std::vector<std::string> notCoveredAtAll;
};

// source 用來讀取文件層級的中繼資料（/Lang、/Info /Title、/Annots 的顏色）；
// tree 是已經讀出的結構樹，避免重複解析。pageCount 用來限定要檢查閱讀順序的頁數範圍
// （沒有結構的頁面不會被結構樹提及，仍然要跑閱讀順序比對的頁只會是樹裡實際出現過的）。
[[nodiscard]] A11yReport runAccessibilityCheck(const PdfSourceDocument& source,
                                               const StructTree& tree);

}  // namespace alioth::engine::objects
