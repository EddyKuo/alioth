#include "engine/pdfium_lock.h"

namespace alioth::engine {

std::recursive_mutex& pdfiumMutex() {
    // 函式區域靜態：初始化是執行緒安全的，而且不依賴翻譯單元的初始化順序。
    // 各子系統的執行緒在建構時就可能搶著取用它。
    static std::recursive_mutex mutex;
    return mutex;
}

}  // namespace alioth::engine
