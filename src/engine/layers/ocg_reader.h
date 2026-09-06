#pragma once

// 圖層（OCG）面板結構的讀取（PRD-VIEW-008，WBS 2.11）。
//
// 刻意不連結 PDFium：/OCProperties 是文件目錄（Catalog）底下的一個字典，
// 我們已有的物件剖析器（Alioth::objects，見 ADR-002）就讀得到，不需要為了
// 讀一個字典再開一份 PDFium 文件把手、佔用僅有的一條 PDFium 執行緒。
//
// 已測過的 PDFium 能力邊界（M0，PRD 第 12 章開放問題之一）：
// third_party/pdfium/include 全部標頭裡沒有任何 OCG 可見性 setter，
// FPDF_RenderPageBitmap 系列一律用文件內建的 /OCProperties /D 預設值渲染，
// 無法在執行期依使用者勾選狀態重新渲染。因此這裡解析出的 OcgTree 只驅動
// UI 面板，不接回渲染管線；詳見 domain/ocg.h 開頭的說明與
// exceptions/EXC_20260906_RD_SA_ocg_render_gap.md。

#include <string>

#include "domain/ocg.h"

namespace alioth::engine::layers {

// 從完整檔案位元組解析。純函數，PDF 是不可信任輸入：任何語法錯誤或找不到
// /OCProperties 一律回傳 present = false 的空樹，不丟例外。
[[nodiscard]] domain::OcgTree readOcgTree(const std::string& bytes);

// 便利入口：從路徑載入（走 platform::SharedReadFile，與存檔用的共享讀取
// 控制代碼同一套，允許他人同時刪除/更名該檔案）。開檔失敗回傳空樹。
[[nodiscard]] domain::OcgTree loadOcgTree(const std::string& utf8Path);

}  // namespace alioth::engine::layers
