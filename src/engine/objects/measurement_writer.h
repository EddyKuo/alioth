#pragma once

// 清除量測註解的顯示值（PRD-ANN-025）。
//
// 「清除距離註解的測量值」在 Acrobat／PDF-XChange 的行為是把量測工具算出來
// 並快取在 /Contents 的標籤文字（例如 "12.3 mm"）移除，但保留幾何與
// /Measure——比例校正與畫出來的線／多邊形都還在，只是不再顯示一個
// 可能已經過期的數字（例如比例校正之後，舊註解上原本的數字就不再準確）。
// 因此這裡刻意只動 /Contents，不動 /Measure、不動幾何鍵，也不重寫外觀
// 串流：外觀串流本來就不畫出量測數字本身（數字只在 /Contents／註釋視窗），
// 所以清除顯示值不需要重新產生 /AP。
//
// 走 IncrementalAppender::currentObject／updateObject 而不是新建一個物件：
// 這是「修改既有物件」而不是「新增物件」，ADR-002 把它歸在同一條通道下——
// 增量儲存的意義是新版本的物件蓋掉舊版本在 xref 裡的位置，原檔位元組
// 仍然一個都不動。

#include <string>

#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct MeasurementClearResult {
    bool ok{false};
    std::string diagnostic{};
};

// annotationObjectNumber 必須指向一則註解字典（例如 writeAnnotation() 回傳的
// AnnotationWriteResult::annotationObject，或從既有文件讀出的物件編號）。
[[nodiscard]] MeasurementClearResult clearMeasurementValue(IncrementalAppender& appender,
                                                           int annotationObjectNumber);

}  // namespace alioth::engine::objects
