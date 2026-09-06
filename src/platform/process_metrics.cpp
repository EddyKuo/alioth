#include "platform/process_metrics.h"

#ifdef _WIN32
#include <windows.h>
// psapi.h 必須在 windows.h 之後。
#include <psapi.h>
#endif

namespace alioth::platform {

ProcessMemory currentProcessMemory() noexcept {
    ProcessMemory result;
#ifdef _WIN32
    // PROCESS_MEMORY_COUNTERS_EX 比基本版多了 PrivateUsage（commit）。
    // 用 K32GetProcessMemoryInfo 而不是 psapi.dll 的匯出，是因為前者在 Windows 7 以後
    // 由 kernel32 直接提供，不必額外連結 psapi.lib，也避免 PSAPI_VERSION 的分歧。
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters)) != 0) {
        result.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
        result.peakWorkingSetBytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
        result.privateBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
        result.valid = true;
    }
#endif
    return result;
}

}  // namespace alioth::platform
