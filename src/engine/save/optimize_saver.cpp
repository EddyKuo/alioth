#include "engine/save/optimize_saver.h"

namespace alioth::engine::save {

OptimizeResult optimizeDocument(DocumentHandle document, const std::string& targetPath,
                                std::int32_t signatureCount, const OptimizeOptions& options) {
    OptimizeResult result;

    if (signatureCount > 0 && !options.acknowledgeSignatureLoss) {
        result.status = OptimizeStatus::RefusedSignaturePresent;
        result.message =
            "文件含 " + std::to_string(signatureCount) +
            " 個數位簽章。最佳化必須整份重寫，會讓既有簽章失效；"
            "已拒絕執行，除非明確確認接受這個後果（OptimizeOptions::acknowledgeSignatureLoss）。";
        return result;
    }

    result.saveResult = IncrementalSaver::saveAsCopy(document, targetPath, SaveOptions{});
    result.status = result.saveResult.ok() ? OptimizeStatus::Ok : OptimizeStatus::Failed;

    result.linearizationApplied = false;
    if (options.requestLinearization) {
        result.linearizationNote =
            "未套用線性化：PDFium 的公開存檔介面不支援線性化輸出，"
            "在目前的技術堆疊下（PRD §4.1 三個引擎級元件）沒有可用的實作路徑。";
    }

    result.message = result.saveResult.ok()
                         ? "已移除未使用物件並整份重寫（" +
                               std::to_string(result.saveResult.metrics.totalBytes) + " 位元組）"
                         : "最佳化失敗：" + result.saveResult.message;
    return result;
}

}  // namespace alioth::engine::save
