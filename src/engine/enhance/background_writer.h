#pragma once

// 新增背景（WBS 14，PRD-ENH-001）。
//
// 背景與浮水印在 PDF 裡是同一種東西——一段畫在頁面上的內容——差別只有
// **順序**：背景插在既有內容之前，浮水印接在之後。順序寫反的症狀是
// 背景把整頁文字蓋掉（不透明的背景）或看起來完全沒生效（半透明的背景），
// 兩者都不會有任何錯誤訊息，因此這件事必須由測試盯著。
//
// 寫入走 IncrementalAppender：加背景是可逆的裝飾，沒有理由為它犧牲既有簽章。

#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::enhance {

struct BackgroundResult {
    bool ok{false};
    std::string diagnostic{};
    std::vector<std::int32_t> pagesChanged{};
    int imageObject{0};  // 影像背景時的 XObject 編號，供測試與診斷追蹤
};

// 對 spec.pages 指定的頁面加上背景（空代表全部頁面）。
//
// 影像背景只解碼與編碼一次，所有頁面共用同一個 XObject：每頁各存一份的話，
// 一份 100 頁的文件會把同一張圖塞進去 100 次。
[[nodiscard]] BackgroundResult addBackground(
    objects::IncrementalAppender& appender, const domain::enhance::BackgroundSpec& spec,
    const domain::enhance::CompressionSettings& compression = {});

}  // namespace alioth::engine::enhance
