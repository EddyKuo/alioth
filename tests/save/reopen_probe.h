#pragma once

// 用引擎轉接層重新開啟存檔結果，驗證它真的還是一份可讀的 PDF。
//
// 為什麼不直接用 save::ScopedDocument 驗：那會用同一份程式碼證明自己是對的。
// 走 PdfiumEngine 這條完全獨立的路徑，才算是第三方視角的驗證。
//
// 呼叫前必須確保沒有任何 ScopedDocument 存活：PdfiumEngine 解構時會呼叫
// FPDF_DestroyLibrary，把此刻仍開著的文件把手一併拆掉。

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

#include "engine/pdfium_engine.h"

namespace alioth::test {

// 回傳頁數；開檔失敗回傳 nullopt。
inline std::optional<int> probePageCount(const std::string& path) {
    alioth::engine::PdfiumEngine engine;

    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    alioth::engine::OpenResult result;

    engine.openDocument(path, "", [&](alioth::engine::OpenResult r) {
        {
            const std::lock_guard lock(mutex);
            result = std::move(r);
            ready = true;
        }
        cv.notify_all();
    });

    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(10), [&] { return ready; })) {
            return std::nullopt;
        }
    }
    if (!result.ok()) return std::nullopt;
    return result.info.pageCount;
}

}  // namespace alioth::test
