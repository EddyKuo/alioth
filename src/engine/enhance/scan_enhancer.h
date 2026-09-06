#pragma once

// 掃描頁去斜與增強的 PDF 側（WBS 14，PRD-ENH-002）。
//
// 流程是「渲染整頁 → 偵測傾斜 → 旋轉校正 → 對比亮度／二值化 → 寫回成一張圖」。
//
// 為什麼要先渲染整頁，而不是直接找出頁面上那張掃描影像來改：
// 掃描件在實務上有兩種形態——單一整頁影像，以及「影像 + OCR 文字層 + 邊框」。
// 後者若只改影像，文字層與影像會錯開，選取範圍全部偏掉。統一走渲染的代價是
// 把可能存在的文字層變成點陣（與點陣化相同的取捨），收益是**行為可預期**：
// 使用者看到的畫面就是輸出的畫面。這件事要在 UI 上說清楚，
// 不能讓使用者以為只是調了一下對比。
//
// 這條路徑同樣是整頁光柵化的合法例外，理由見 page_rasterizer.h。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::enhance {

struct ScanEnhanceSettings {
    // 掃描件的原生解析度通常是 200–300 dpi，取 200 是可讀性與體積的平衡點。
    // 低於原生解析度會讓細筆畫斷開，而我們沒有辦法知道原生解析度是多少——
    // 影像的像素數要配上它在頁面上的擺放矩陣才算得出來，那需要重放內容串流。
    double dpi{200.0};

    bool deskewEnabled{true};
    domain::enhance::DeskewSettings deskew{};
    domain::enhance::EnhanceSettings enhancement{};
    domain::enhance::CompressionSettings compression{};

    // 旋轉後露出的角落填什麼。掃描件的紙張是白的，填白才不會在邊緣出現黑角。
    std::uint8_t deskewBackground{255};
};

struct PageEnhanceReport {
    std::int32_t pageIndex{0};
    bool deskewApplied{false};
    double angleDeg{0.0};
    std::string deskewNote{};  // 沒有校正時的原因，必須讓使用者看得到
    std::int64_t imageBytes{0};
};

struct ScanEnhanceResult {
    bool ok{false};
    std::string diagnostic{};
    std::vector<PageEnhanceReport> pages{};
};

// 對 pages 指定的頁面（空代表全部）套用去斜與增強。
[[nodiscard]] ScanEnhanceResult enhanceScannedPages(objects::IncrementalAppender& appender,
                                                    const std::vector<std::int32_t>& pages,
                                                    const ScanEnhanceSettings& settings);

}  // namespace alioth::engine::enhance
