#pragma once

// 色彩轉換與 Recolor（PRD-ENH-006，WBS 14）。
//
// 覆蓋範圍與明確排除的部分寫在 domain/enhance.h 的 ColorTransformSettings
// 註解裡（那是唯一真相來源，這裡不重複）。實作分兩條完全獨立的路徑：
//
//   - 影像 XObject：解碼成像素、逐像素套用 domain::enhance::applyColorTransform、
//     重編碼、就地覆寫物件。走的路徑與 image_recompressor.cpp 幾乎相同，
//     這是刻意的相似——兩者都是「走頁面找 XObject、換內容、寫回去」。
//   - 內容串流的裝置色彩運算子：以最小的內容串流語彙分析器（僅辨識 token
//     邊界，不建圖形狀態機）找出 g/G/rg/RG/k/K/sc/scn 等運算子與其前面的
//     數字運算元，整段位元組替換成新的顏色運算子。非目標 token 原樣照抄，
//     因此排版、路徑、文字位置完全不受影響。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/enhance.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::enhance {

struct ColorTransformReport {
    // 影像側。
    int imagesTransformed{0};
    int imagesSkippedUnsupported{0};  // 索引色、CMYK 原始樣本、色鍵遮罩等
    int imagesFailed{0};

    // 內容串流側。
    int contentOperatorsRewritten{0};
    // sc/scn 運算元數量不是 1/3/4，或前面帶了色彩空間名稱／Pattern 名稱
    // （代表用了本功能不追蹤的色彩空間），一律計入這裡而不是靜默跳過。
    int contentOperatorsSkippedUnsupported{0};
    // 內容串流本身有改寫內容，但寫回附加器失敗（理論上只會在物件編號
    // 衝突時發生）。計入這裡而不是吞掉，呼叫端至少能看出「有些頁面沒轉成」。
    int contentStreamsFailed{0};
};

struct ColorTransformResult {
    bool ok{false};
    std::string diagnostic;
    ColorTransformReport report;
};

[[nodiscard]] ColorTransformResult convertColors(objects::IncrementalAppender& appender,
                                                 const domain::enhance::ColorTransformSettings& settings);

// 內容串流層的轉換，抽出來單獨測試：輸入輸出都是未壓縮的內容串流位元組，
// 不需要一份完整的 PDF 就能驗證運算子有沒有被正確辨識與改寫。
struct ContentColorTransformResult {
    std::string content;
    int rewritten{0};
    int skippedUnsupported{0};
};

[[nodiscard]] ContentColorTransformResult transformContentStreamColors(
    const std::string& content, const domain::enhance::ColorTransformSettings& settings);

}  // namespace alioth::engine::enhance
