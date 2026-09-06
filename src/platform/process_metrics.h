#pragma once

// 行程資源量測（WBS 7.4，PRD §8.1 的記憶體指標）。
//
// PRD §8.1 把記憶體列為驗收條件：500 頁工程圖閒置 ≤ 512 MB、10,000 頁文字型 ≤ 768 MB。
// 要量它就必須問作業系統，而作業系統呼叫只能出現在平台層（CLAUDE.md 分層規則）——
// 效能基準工具因此只呼叫本介面，不得自行 include <psapi.h>。
// 這條規則的代價現在看起來只是多一層轉接，等到移植 macOS / Linux 時，
// 要改的就只有本檔的實作，而不是散在 tools/ 與 tests/ 各處的 #ifdef。
//
// 「工作集」（working set）是 Windows 上與工作管理員「記憶體」欄位最接近的量，
// 也是 PRD 驗收時使用者會看的那個數字，因此以它為主要指標。
// 私有位元組（private bytes / commit）另外提供：它排除了共用的 DLL 頁面，
// 比較不受 Qt 與 PDFium 的映射影響，適合追蹤我們自己的配置行為。

#include <cstdint>

namespace alioth::platform {

struct ProcessMemory {
    std::uint64_t workingSetBytes{0};      // 目前工作集
    std::uint64_t peakWorkingSetBytes{0};  // 行程生命期內的工作集峰值
    std::uint64_t privateBytes{0};         // 私有（commit）位元組
    // 取得失敗時為 false。呼叫端必須處理：量不到記憶體要明說，
    // 不得用 0 冒充「很省」——那正是 CI 上靜默通過的典型來源。
    bool valid{false};
};

// 取得目前行程的記憶體用量。失敗時回傳 valid = false 的結果，不丟例外。
[[nodiscard]] ProcessMemory currentProcessMemory() noexcept;

// 為什麼不提供「先修剪工作集再量」的版本
//
// 直覺的做法是先呼叫 SetProcessWorkingSetSize(-1, -1) 把工作集歸還給作業系統，
// 再量一次，理由是 Windows 不會主動縮減工作集。實測（本專案 20 頁 A0 語料）
// 那樣量到的是 0.2 MB——它把整個工作集清空了，量到的不是「閒置需求」而是
// 「什麼都不在記憶體裡」，接著所有頁面又會以分頁錯誤逐一回來。
// 那個數字看起來很漂亮，但它不對應使用者在工作管理員看到的任何東西，
// 而 PRD §8.1 的驗收正是對著那個欄位。因此本模組只如實回報，不做修剪。
//
// 對應地，預算判定應以 privateBytes（commit）為準：它不受工作集修剪與
// 共用 DLL 映射影響，是最能反映「我們自己配置了多少」的量。
// workingSetBytes 一併回報，供與工作管理員對照。

}  // namespace alioth::platform
