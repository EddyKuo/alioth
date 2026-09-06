#pragma once

// FDF 匯入／匯出（PRD-ANN-013 的另一半）。
//
// FDF（ISO 32000-1 §12.7.7）與 XFDF 帶的是同一批資料，差別只在外殼：FDF 用
// PDF 的物件語法，XFDF 用 XML。兩者並存不是重複——Acrobat 的「匯出註解」預設
// 給的是 FDF，而許多既有的審閱流程（尤其是舊版工具產出的檔案）只認得它。
// 收到一份 .fdf 卻只能說「請對方改存成 XFDF」，等於這條需求沒做。
//
// 因此這裡刻意共用 XfdfEntry：兩種格式的語意完全相同，各自定義一份結構只會
// 讓「同一則註解」在程式裡有兩種形狀，轉換程式碼會長在呼叫端。
//
// 安全立場與 XFDF 一致（FDF 同樣來自信件附件，不比 PDF 可信）：
//   - 大小上限，避免單純的巨大檔案先耗盡記憶體
//   - **不執行也不保留 /JavaScript**。FDF 容器合法地可以帶 JavaScript
//     （§12.7.7.3 的 /JavaScript 鍵），而本產品的安全立場是不執行任何
//     PDF 內嵌 JavaScript；這裡連讀都不讀，遇到就記進診斷讓使用者知道
//     那份檔案帶了什麼，而不是靜默丟掉。
//   - 不處理 /F 指向的檔案路徑，只當成顯示用的提示字串——跟著它去開檔
//     等於讓一份附件決定要讀本機的哪個檔案。
//
// 支援的子集與 XFDF 相同（文字標記四種、Square、Circle、Line、Ink、Text、
// FreeText、Caret、Polygon、PolyLine）。不支援的子型一律跳過並列進診斷。

#include <cstddef>
#include <string>
#include <vector>

#include "app/xfdf_io.h"

namespace alioth::app {

// FDF 與 XFDF 攜帶同一批資料，共用同一個項目結構。
using FdfEntry = XfdfEntry;

// 輸入大小上限（與 XFDF 相同的量級）。超過一律拒絕。
inline constexpr std::size_t kMaxFdfBytes = 10u * 1024u * 1024u;

// 序列化成 FDF 位元組。sourceFilename 對應 /F，純粹是給收件人辨識用的提示。
[[nodiscard]] std::string exportFdf(const std::vector<FdfEntry>& entries,
                                    const std::string& sourceFilename = {});

struct FdfImportResult {
    bool ok{false};
    std::string diagnostic;
    std::vector<FdfEntry> entries;
    std::vector<std::string> skipped;  // 跳過的子型或無法解析的物件說明
};

[[nodiscard]] FdfImportResult importFdf(const std::string& bytes);

}  // namespace alioth::app
