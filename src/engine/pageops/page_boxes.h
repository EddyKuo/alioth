#pragma once

// 設定文件邊界（PRD-PAGE-010）與正規化頁面（PRD-PAGE-011），WBS 12。
//
// 五種框的關係：/MediaBox 是紙張、/CropBox 是檢視與列印時的可見範圍、
// /BleedBox 出血、/TrimBox 裁切成品尺寸、/ArtBox 有意義的內容範圍。
// 後四者依規格都必須落在 /MediaBox 之內；越界時各家檢視器的處理並不一致，
// 因此預設夾進去（見 domain::compose::PageBoxSettings::clampToMedia）。
//
// 正規化是本工作包最容易寫錯的一項。真實檔案的 /MediaBox 原點常常不是 (0,0)
// （掃描器、排版軟體、裁切過的工程圖都會產生這種檔案），而幾乎所有下游程式碼
// 都假設它是。正規化的定義是**平移整個座標系**，不是把框的數字改小：
//
//   1. /MediaBox 移到 (0,0)
//   2. 其餘四個框平移同樣的量
//   3. 頁面內容前面插入對應的 cm（並用 q/Q 包住，否則平移會外溢）
//   4. 所有註解的幾何平移同樣的量
//
// 少做第 3 步 → 內容整個偏移；少做第 4 步 → 頁面看起來完全正確、標記卻跑掉。
// 後者是最貴的一種：它在視覺上沒有任何徵兆，只有把註解點開才會發現。

#include <string>
#include <vector>

#include "domain/page_compose.h"
#include "engine/pageops/compose_document.h"

namespace alioth::engine::pageops {

struct PageBoxRequest {
    std::vector<int> pages;  // 留空代表全部頁面
    domain::compose::PageBoxSettings settings{};
};

struct PageBoxResult : PageOpsResult {
    int changedPages{0};
    bool clamped{false};  // 有子框被夾進 MediaBox，UI 應該讓使用者知道
};

[[nodiscard]] PageBoxResult setPageBoxes(std::string sourceBytes, const PageBoxRequest& request);

struct NormalizeRequest {
    std::vector<int> pages;  // 留空代表全部頁面
    bool moveAnnotations{true};
};

struct NormalizeResult : PageOpsResult {
    int normalizedPages{0};
    int movedAnnotations{0};
};

[[nodiscard]] NormalizeResult normalizePages(std::string sourceBytes,
                                             const NormalizeRequest& request);

// 每一頁的可見尺寸（/CropBox，含 /Rotate 之後的長寬互換），依頁序。
//
// 給的是「這一頁看起來多大」，不是 /MediaBox 的原始數字：要把一頁放進版面裡，
// 需要的是它顯示出來的形狀。/Rotate 90 的 A4 直向頁在版面上是橫的，
// 用未旋轉的尺寸去算格子，結果是內容超出格子邊界或縮得過小。
//
// 讀不開的檔案回傳空 vector——呼叫端本來就會先因為別的錯誤停下來。
[[nodiscard]] std::vector<domain::SizeF> readVisiblePageSizes(const std::string& bytes);

}  // namespace alioth::engine::pageops
