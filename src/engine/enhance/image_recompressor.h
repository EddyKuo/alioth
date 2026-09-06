#pragma once

// 影像重壓縮（WBS 14，PRD-ENH-004）。
//
// 三件事決定了這個功能的形狀：
//
//   1. **重壓後變大是常態，不是例外。** 原圖若已經是高品質 JPEG，解一次再編
//      一次幾乎必然變大；已經是 CCITT / JBIG2 的黑白掃描件轉成 JPEG 更是
//      好幾倍。因此「變小才替換」是正確性要求：使用者按下重壓縮之後檔案變大，
//      那是產品缺陷。沒被替換的影像仍然要出現在報告裡，
//      不能靜默略過——使用者需要知道為什麼沒省到。
//
//   2. **遮罩必須跟著走。** PDF 的影像沒有 alpha 通道，透明度靠 /SMask
//      （另一張同尺寸的灰階影像）。只換彩色資料而留下舊的 /SMask，
//      尺寸一旦不同透明度就整片錯位；重取樣之後尺寸一定不同。
//      這裡的作法是把 /SMask 併回 alpha、一起重編，再寫出新的 /SMask。
//
//   3. **不認得的形態要明確跳過。** 索引色、CMYK、1 位元、色鍵遮罩
//      （/Mask 是陣列）都會在重新編碼後失去意義——色鍵記的是原始取樣值，
//      JPEG 一壓那些值就變了，遮罩會開始挖掉不該挖的地方。
//      這些一律回報 SkippedUnsupported，不猜。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::enhance {

struct RecompressResult {
    bool ok{false};
    std::string diagnostic{};
    std::vector<domain::enhance::ImageRecompressReport> images{};

    [[nodiscard]] std::int64_t originalBytes() const noexcept {
        std::int64_t total = 0;
        for (const auto& image : images) total += image.originalBytes;
        return total;
    }
    [[nodiscard]] std::int64_t savedBytes() const noexcept {
        std::int64_t total = 0;
        for (const auto& image : images) total += image.savedBytes();
        return total;
    }
    [[nodiscard]] std::size_t replacedCount() const noexcept {
        std::size_t count = 0;
        for (const auto& image : images) {
            if (image.decision == domain::enhance::RecompressDecision::Replaced) ++count;
        }
        return count;
    }
};

// 重壓 pages 指定頁面（空代表全部）用到的影像 XObject。
//
// 同一個影像物件被多頁共用時只處理一次：重複處理不只是浪費，
// 第二次會拿已經壓過的結果再壓一次，畫質白白掉一代。
[[nodiscard]] RecompressResult recompressImages(
    objects::IncrementalAppender& appender, const std::vector<std::int32_t>& pages,
    const domain::enhance::RecompressSettings& settings);

}  // namespace alioth::engine::enhance
