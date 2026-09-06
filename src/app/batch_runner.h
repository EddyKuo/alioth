#pragma once

// 批次處理執行器（PRD-MAC-001，WP35）。
//
// 巨集本身（domain/macro.h）只是資料；這裡是唯一會真的打開檔案、呼叫既有
// 服務、寫出新檔案的地方。三條判準來自 WP35 的工作包說明，直接對應成
// 這個類別的行為：
//
//   1. 批次處理是不可逆操作的放大器（一次錯就是 N 個檔案），所以一定要有
//      dry-run：列出將要改哪些檔案、依序做什麼，不碰檔案系統。
//   2. 輸出預設寫到新檔案，不覆蓋原檔。formatOutputPath 保證輸出路徑與
//      輸入路徑不同；萬一使用者提供的樣板算出相同路徑，run() 會明確失敗
//      而不是靜默覆蓋。
//   3. 每個動作對應一個既有的、已經測過的服務呼叫（見 domain/macro.h 開頭
//      的說明），這裡的職責只是照順序呼叫它們並在檔案之間傳遞位元組。
//
// 執行順序刻意是**單一執行緒、逐檔案循序處理**，不做任何平行化：
// SDD §1.1 記錄了「四個工作者同時開同一份檔案」會漏頁的未解問題，而批次
// 處理的下一步是對同一份檔案連續呼叫兩個不同的 PDFium 子系統（PageEditor
// 與 IncrementalAppender），穩妥的作法是完全不重疊，等一個動作、一份檔案
// 徹底做完再開始下一個。200 份檔案循序處理比並行處理更慢，但正確性優先
// ——這與批次處理「一次錯就是一大片」的風險特性一致。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/macro.h"

namespace alioth::app {

struct BatchFileResult {
    std::string inputPath;
    std::string outputPath;  // dry-run 或失敗時仍會填，代表「將會／原本要」寫到哪裡
    bool ok{false};
    std::string diagnostic;
    // 每個步驟一則描述，dry-run 與正式執行都會填，方便使用者核對兩者一致。
    std::vector<std::string> stepDescriptions;
};

struct BatchRunResult {
    bool ok{false};  // 巨集本身不合法時為 false 且 files 為空
    std::string diagnostic;
    std::vector<BatchFileResult> files;

    [[nodiscard]] std::size_t successCount() const noexcept {
        std::size_t count = 0;
        for (const BatchFileResult& file : files) count += file.ok ? 1 : 0;
        return count;
    }
};

struct BatchRunOptions {
    // "{name}" 換成不含副檔名的原檔名（不含路徑），"{ext}" 換成副檔名
    // （含點；沒有副檔名時為空）。輸出永遠與輸入同一個目錄——批次處理的
    // 使用情境通常是「這個資料夾裡的兩百份檔案」，換目錄的決定留給呼叫端
    // 事後自行搬移，不在這裡做路徑猜測。
    std::string outputPattern{"{name}_batch{ext}"};
    bool dryRun{true};
};

class BatchRunner {
public:
    [[nodiscard]] static std::string formatOutputPath(const std::string& inputPath,
                                                       const std::string& pattern);

    // 對每個輸入檔案依序套用同一份巨集。巨集驗證失敗時整批直接回報，
    // 不會嘗試「先跑能跑的部分」——那會讓使用者以為巨集是對的，
    // 直到某個特定動作剛好觸發驗證失敗的組合才發現。
    [[nodiscard]] BatchRunResult run(const domain::macro::MacroDefinition& macro,
                                     const std::vector<std::string>& inputPaths,
                                     const BatchRunOptions& options = {}) const;
};

}  // namespace alioth::app
