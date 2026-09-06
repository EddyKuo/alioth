#pragma once

// 讀取既有註解字典的 /Measure（ISO 32000-1 §12.5.6.11，ADR-002 的讀取側）。
//
// PRD-ANN-024 的「校正」是給新量測用的；但一份文件可能已經被 Acrobat 或
// PDF-XChange 校正過並存在既有的 Line/Polygon/PolyLine 註解上，量測工具
// 必須認得那份既有比例，不能每次都要求使用者重新校正。
//
// 找不到 /Measure、或找到但格式看不懂、或 X/Y 兩軸比例不一致（非等向縮放）
// 一律回傳「未校正」加明確原因，絕不回退到假設 1:1——CLAUDE.md 對這件事的
// 措辭是「絕對不要」，因為看起來合理的錯誤數字比明顯的「未校正」提示更危險。

#include <string>

#include "domain/measurement.h"
#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::objects {

struct MeasureReadResult {
    bool calibrated{false};
    std::string diagnostic{};       // calibrated 為 false 時的原因
    domain::MeasureInfo measure{};  // calibrated 為 true 時才有效
    bool hasAreaFormat{false};      // /Measure 是否帶 /A（該註解可換算面積）
};

// annotationDict 必須是已經解出的註解字典物件（呼叫端負責把間接參照解到
// 註解本身；這裡只處理 /Measure 鍵本身可能是間接參照的情況）。
[[nodiscard]] MeasureReadResult readMeasure(const PdfSourceDocument& source,
                                            const PdfObject& annotationDict);

}  // namespace alioth::engine::objects
