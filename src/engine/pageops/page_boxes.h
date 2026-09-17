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

// 頁面尺寸調整（PRD-PAGE-003 的第三項）。
//
// 「改紙張大小」有兩種完全不同的意思，而使用者想要哪一種**不能猜**：
//
//   ScaleContent — 內容跟著等比縮放填進新紙張。把 A4 的報告印成 A3 時要的
//                  就是這個；比例尺會變，所以工程圖不該用它。
//   KeepContent  — 內容維持原尺寸，只換紙張並置中。工程圖換紙時必須用這個：
//                  圖面上標的 1:100 是紙上的事實，縮放過的圖再量就是錯的。
//                  新紙比內容小時，超出的部分會被 MediaBox 裁掉——那是這個
//                  選項的語意本身，UI 必須先講清楚。
//
// 兩者都走 Form XObject 重新包裝，所以註解會套用同一個矩陣一起搬；
// 只改 /MediaBox 的做法（setPageBoxes）不動內容也不動註解，是第三種語意，
// 不在這裡。
enum class ResizePolicy : std::uint8_t {
    ScaleContent,
    KeepContent,
};

struct ResizePagesRequest {
    std::vector<int> pages;      // 留空代表全部頁面
    domain::SizeF pageSize{};    // 目標紙張（點）
    ResizePolicy policy{ResizePolicy::ScaleContent};
    double marginPt{0.0};        // 內容四周留白（ScaleContent 才有意義）
    bool moveAnnotations{true};
};

struct ResizePagesResult : PageOpsResult {
    int resizedPages{0};
    int movedAnnotations{0};
};

[[nodiscard]] ResizePagesResult resizePages(std::string sourceBytes,
                                            const ResizePagesRequest& request);

}  // namespace alioth::engine::pageops
