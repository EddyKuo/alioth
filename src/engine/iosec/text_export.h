#pragma once

// 匯出純文字（PRD-IO-007 的後半）。
//
// 走文字子系統既有的 TextExtractor：它是唯一被授權在頁面層級擷取文字的入口
// （獨立文件把手 + 專用執行緒，CLAUDE.md 硬性限制 1）。本檔只是把逐頁文字
// 串接、頁與頁之間插入分隔字元，寫成 UTF-8 檔案，不重新實作擷取邏輯。
//
// 一次只排一頁工作，模式與 engine::text::SearchSession 相同：整份文件一次
// 全排進佇列的話，呼叫端想中途放棄就要等佇列跑完；逐頁排的取消延遲最壞
// 也只多一頁。
//
// 呼叫端必須先讓 TextExtractor 完成 open()：本函式不管理它的生命週期，
// 也不負責開檔錯誤處理——那是呼叫端已經在其他路徑（例如開啟文件時）做過
// 的事，這裡重做一次只會製造第二個「開檔失敗」的判斷位置（IL-3）。

#include <cstdint>
#include <functional>
#include <string>

#include "engine/text/text_extractor.h"

namespace alioth::engine::iosec {

enum class TextExportStatus {
    Ok,
    NoPages,      // 文件已開啟但沒有頁面可匯出
    WriteFailed,
};

[[nodiscard]] const char* describe(TextExportStatus status) noexcept;

struct TextExportResult {
    TextExportStatus status{TextExportStatus::NoPages};
    std::string message;
    std::int32_t pagesExported{0};

    [[nodiscard]] bool ok() const noexcept { return status == TextExportStatus::Ok; }
};

// 匯出整份文件的純文字。extractor 必須已經 open() 成功。
// pageBreak 是頁與頁之間插入的分隔字元，預設換頁字元（\f）—— 與 Acrobat
// 「儲存為文字」的既有慣例一致，讓匯出檔在支援分頁字元的檢視器裡也能分頁顯示。
//
// 非同步：完成時在文字執行緒上呼叫 callback，呼叫端須自行排回自己的執行緒
// （與 TextExtractor 本身的規則相同）。
void exportDocumentText(text::TextExtractor& extractor, const std::string& outputPath,
                        std::function<void(TextExportResult)> callback,
                        std::string pageBreak = "\f");

}  // namespace alioth::engine::iosec
