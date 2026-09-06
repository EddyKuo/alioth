#pragma once

// 自動儲存與異常復原（PRD-IO-003、WBS 5.3）。
//
// 三個不可退讓的性質：
//
// 一、自動儲存永遠寫到 platform::autosaveDirectory() 下的獨立檔案，絕不碰使用者原檔。
//     使用者沒有按下儲存，就代表他還沒決定要不要接受這些變更；替他決定是資料損毀。
//
// 二、自動儲存走增量路徑（複製原檔 + 追加變更），不是整份重寫。復原出來的檔案因此
//     與原檔位元組相容，既有簽章的狀態也跟著被保住——復原不該是降級。
//
// 三、側錄檔（sidecar）的存在與否就是「上次是否異常中止」的判準：正常關檔會呼叫
//     clearSession() 把它刪掉，行程被強制中止則不會。所以側錄檔必須在自動儲存檔
//     完整落盤之後才寫，順序反了會讓復原指向一個殘缺的 PDF。
//
// 這裡刻意不內建計時器：引擎層不擁有事件迴圈，節奏由應用層的計時器驅動並呼叫
// isDue()。副作用是自動儲存可以用假時鐘測試，不必真的等兩分鐘。

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/save/incremental_saver.h"

namespace alioth::engine::save {

using AutosaveClock = std::chrono::steady_clock;

// 側錄中繼資料。復原對話框要能在不開啟 PDF 的前提下顯示「哪份檔、什麼時候存的」。
struct AutosaveEntry {
    std::string originalPath;
    std::string autosavePath;
    std::string sidecarPath;
    std::int64_t savedAtEpochMs{0};
    std::int64_t originalModifiedEpochMs{0};
    std::uint64_t originalSizeBytes{0};

    [[nodiscard]] bool valid() const noexcept {
        return !autosavePath.empty() && !originalPath.empty();
    }
};

struct AutosaveResult {
    SaveStatus status{SaveStatus::Ok};
    AutosaveEntry entry{};
    SaveMetrics metrics{};
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return status == SaveStatus::Ok; }
};

class AutosaveManager {
public:
    // PRD-IO-003 明訂預設兩分鐘。
    static constexpr std::chrono::seconds kDefaultInterval{120};

    // 目錄留空代表使用 platform::autosaveDirectory()。指定目錄僅供測試隔離用。
    explicit AutosaveManager(std::string directory = {});

    void setInterval(std::chrono::seconds interval);
    [[nodiscard]] std::chrono::seconds interval() const noexcept { return interval_; }

    // 文件有未存檔變更時呼叫。沒有變更就不該有自動儲存——空轉寫檔會讓
    // 雲端同步資料夾（PRD-IO-005）不停上傳同一份內容。
    void markDirty(AutosaveClock::time_point now = AutosaveClock::now());
    void markClean(AutosaveClock::time_point now = AutosaveClock::now());
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }

    [[nodiscard]] bool isDue(AutosaveClock::time_point now = AutosaveClock::now()) const;

    // 執行一次自動儲存。成功後 dirty 旗標清除、計時重新起算。
    AutosaveResult autosave(DocumentHandle document, const std::string& originalPath,
                            AutosaveClock::time_point now = AutosaveClock::now());

    // 正常關檔或使用者手動存檔後呼叫：刪掉自動儲存檔與側錄檔，
    // 下次啟動就不會把它當成當機殘留提示復原。
    bool clearSession(const std::string& originalPath);

    [[nodiscard]] const std::string& directory() const noexcept { return directory_; }

    // 掃描自動儲存目錄，列出可復原的項目。目錄留空則使用 platform::autosaveDirectory()。
    [[nodiscard]] static std::vector<AutosaveEntry> findRecoverable(const std::string& directory = {});

    // 使用者選擇「放棄復原」時清掉殘留。
    static bool discard(const AutosaveEntry& entry);

    // 原檔路徑 → 自動儲存檔路徑。同一份文件反覆自動儲存會覆寫同一個檔案，
    // 否則長時間編輯會在快取目錄裡堆出上百份副本。
    [[nodiscard]] static std::string autosavePathFor(const std::string& originalPath,
                                                     const std::string& directory);

private:
    std::string directory_;
    std::chrono::seconds interval_{kDefaultInterval};
    AutosaveClock::time_point lastSave_{AutosaveClock::now()};
    bool dirty_{false};
};

}  // namespace alioth::engine::save
