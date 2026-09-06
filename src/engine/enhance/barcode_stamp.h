#pragma once

// 新增條碼（PRD-ENH-007，WBS 14）。
//
// 只支援 Code 128（domain/barcode.h 說明了為什麼不做 QR）。繪製走既有的
// content_stream_appender（ADR-002 的新增內容通道），不建立影像 XObject——
// 條碼是純向量矩形，沒有理由先點陣化再嵌成圖片，那樣做只會讓輸出在高倍率
// 縮放時出現鋸齒，而條碼掃描器對鋸齒邊緣的容忍度比人眼低。
//
// 人讀文字（條碼下方印出的原始內容）走標準 14 字型，因此與專案其餘的
// 純文字繪製一樣，只接受可列印 ASCII；含非 ASCII 內容在 domain::barcode
// 的編碼階段就已經被擋下，這裡不會再遇到。

#include <string>

#include "domain/barcode.h"
#include "domain/geometry.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::enhance {

struct BarcodeStampOptions {
    // 條碼「有墨」區域左右各保留的靜區寬度，以模組數表示。Code 128 規格建議
    // 至少 10 個模組；這裡的 rect 是靜區以外的可視框，實際畫墨的區域會再往
    // 內縮這個寬度換算出的點數。
    int quietZoneModules{10};

    // 是否在條碼下方印出人讀文字（原始內容）。掃描器讀不出來時，人眼要有
    // 辦法核對內容，這是條碼類功能的常見要求，不是裝飾。
    bool includeHumanReadableText{true};
    double humanReadableFontSize{8.0};
};

struct BarcodeStampResult {
    bool ok{false};
    std::string diagnostic;
    int contentObject{0};
};

// 在指定頁面貼上一枚 Code 128 條碼。rect 是整個貼圖框（含靜區），頁面座標。
[[nodiscard]] BarcodeStampResult stampBarcode(objects::IncrementalAppender& appender,
                                              int pageIndex, const std::string& text,
                                              const domain::RectF& rect,
                                              const BarcodeStampOptions& options = {});

}  // namespace alioth::engine::enhance
