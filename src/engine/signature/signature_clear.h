#pragma once

// 清除所有簽章欄位（對標 PDF-XChange 的 Protect → Clear all Signatures）。
//
// 用途只有一個：拿一份簽過的文件當範本，把簽章拿掉再重新走一次流程。
//
// **這不是「讓文件回到未簽署的狀態」**，而且兩者的差別很重要：
//
// 寫入走的是與其他修改相同的增量附加通道（ADR-002），所以被移除的只是
// **參照**——/Sig 字典、憑證與被簽的位元組全部還留在檔案裡，只是沒有任何
// 地方指向它們。任何能讀 PDF 修訂版的工具都還找得回來。想要真的不留痕跡，
// 必須接著走一次全檔重寫（另存新檔的最佳化路徑會丟掉不可達物件）。
//
// 選增量而不是直接全檔重寫，是因為這一層不該替使用者決定「順便重寫整份檔案」：
// 全檔重寫會讓文件裡**其他**簽章也一起失效，而使用者要清的可能只是其中一個
// 流程留下的欄位。呼叫端該做的是把這件事講清楚，而不是替他選。
//
// 只動兩個陣列：catalog 的 /AcroForm /Fields 與各頁的 /Annots。
// 被指向的物件一個都不刪——附加式寫入本來就不能刪東西，而留著它讓復原
// 仍然只是把檔案截回原長度。

#include <string>

namespace alioth::engine::signature {

struct ClearSignaturesResult {
    bool ok{false};
    std::string diagnostic;

    int removedFields{0};   // 從 /AcroForm /Fields 摘掉的欄位
    int removedWidgets{0};  // 從頁面 /Annots 摘掉的 widget
    std::string bytes;      // 成功時的完整輸出（原檔 + 增量段）

    [[nodiscard]] bool changedAnything() const noexcept {
        return removedFields > 0 || removedWidgets > 0;
    }
};

[[nodiscard]] ClearSignaturesResult clearSignatureFields(const std::string& sourceBytes);

}  // namespace alioth::engine::signature
