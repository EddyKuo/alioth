#pragma once

// 另存新檔與最佳化（PRD-IO-002）。
//
// 「移除未使用物件」與「線性化」在目前的技術堆疊下可行性不同，分開陳述：
//
//   移除未使用物件 —— 可行，而且不需要新程式碼。IncrementalSaver::saveAsCopy
//   已經是整份重寫（FPDF_NO_INCREMENTAL）：PDFium 的序列化器從 /Root 開始做
//   可達性標記再輸出，不可達的物件本來就不會被寫進輸出檔（實測見
//   tests/iosec/test_optimize_saver.cpp 的 unreachable object 案例，不是憑
//   PDFium 原始碼推測——這條假設如果哪天不成立，測試會先紅）。
//
//   線性化 —— 不可行。PRD 附錄 C 列出的 PDFium 存檔介面只有
//   FPDF_SaveWithVersion / FPDF_SaveAsCopy，兩者都沒有線性化選項；PDFium 的
//   公開建置也從未提供線性化寫出器（線性化通常由 qpdf --linearize 這類獨立
//   工具事後對整份檔案重排）。引入 qpdf 作為執行期相依會違反 PRD §4.1
//   「Qt 6 Widgets + PDFium + OpenSSL 3.x，三件而已」的技術堆疊上限（qpdf
//   目前只以建置期／測試期工具的身份存在，見 tests/qa/qpdf_check.h）。
//   因此本檔的最佳化路徑「線性化」欄位恆為未套用，並在結果裡明確說明原因，
//   不假裝完成。
//
// 這裡最重要的行為是簽章防呆：optimizeDocument 一律整份重寫，會讓既有數位
// 簽章失效（物件編號全部重排）。呼叫端必須先查出文件是否有簽章
// （FPDF_GetSignatureCount 或 Alioth::signature）並傳進來；有簽章時，
// 除非呼叫端明確設定 acknowledgeSignatureLoss，否則一律拒絕執行——
// 這是「重寫與增量儲存互斥」在介面上的具體實作，不是靠呼叫端記得先問。

#include <cstdint>
#include <string>

#include "engine/save/incremental_saver.h"

namespace alioth::engine::save {

enum class OptimizeStatus {
    Ok,
    RefusedSignaturePresent,  // 文件有簽章且呼叫端未確認要接受簽章失效
    Failed,                   // 底層 saveAsCopy 失敗，細節見內含的 SaveResult
};

struct OptimizeOptions {
    // 移除未使用物件是唯一真正執行的最佳化，因此沒有獨立開關——
    // 它就是「整份重寫」這件事本身，不重寫就沒有移除可言。
    //
    // 呼叫端明確表示「知道這會讓簽章失效，仍要繼續」。UI 層應該在使用者
    // 按下確認鍵之後才把這個旗標設成 true，而不是預設開啟。
    bool acknowledgeSignatureLoss{false};

    // 使用者是否有要求線性化。即使為 true，結果也一定是「不支援」——
    // 保留這個欄位是為了讓呼叫端能把「使用者想要」與「我們做不到」分開記錄，
    // 而不是讓 UI 上的線性化選項悄悄消失、變得像是從來沒有這個需求。
    bool requestLinearization{false};
};

struct OptimizeResult {
    OptimizeStatus status{OptimizeStatus::Failed};
    SaveResult saveResult{};  // status == Ok 時才有意義
    bool linearizationApplied{false};  // 恆為 false，見檔頭「線性化」說明
    std::string linearizationNote;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return status == OptimizeStatus::Ok; }
};

// signatureCount：呼叫端先查出的既有簽章數（0 代表沒有簽章）。
// 這裡不自己去查，是因為查詢需要另一個 PDFium 子系統（Alioth::signature）的
// 獨立文件把手，而 save 這個 target 不依賴 signature——依賴方向永遠只能是
// signature 依賴 save 的輸出可驗證，不能反過來（IL-3：單一真相來源只在
// signature 子系統）。
[[nodiscard]] OptimizeResult optimizeDocument(DocumentHandle document,
                                              const std::string& targetPath,
                                              std::int32_t signatureCount,
                                              const OptimizeOptions& options = {});

}  // namespace alioth::engine::save
