#pragma once

// CSV 灌入表單（PRD-FORM-023）。
//
// 這是批次操作：CSV 每一資料列對應一份輸出 PDF。存在的理由是「填一百份差不多
// 的表單」——例如一百人的報名表——不該要求使用者手填一百次。
//
// 錯誤處理是這個功能唯一容易被做壞的地方，因此明訂兩條規則：
//
//   1. CSV 表頭的欄位名如果在範本裡找不到對應的表單欄位，這是**整份 CSV**
//      的結構問題（每一列都受影響，不是某一列特別壞），所以不算進「跳過某列」
//      的政策，而是整批忽略那一欄並記錄在 report.ignoredColumns。
//      如果一欄都對不上，代表使用者可能貼錯範本，直接中止整批、不寫出任何檔案。
//   2. 某一資料列的欄位數與表頭不一致（CSV 本身格式有問題，例如漏打逗號）
//      才是真正「某一列」的問題，依 CsvMismatchPolicy 決定中止整批或跳過該列，
//      兩種都合理，但必須明確、可設定，並在報告裡列出哪幾列出了什麼事——
//      靜默跳過會讓使用者拿到少了幾份的一疊檔案而不自知（IL-4）。
//
// 之所以不能只靠 FormDocument::importData 內建的比對：那支函式對「XFDF 裡
// 有一個欄位名，文件裡找不到」是靜默略過（見 form_document.cpp），這正是
// CLAUDE.md 點名要避免的「靜默跳過」，所以表頭驗證必須在這裡自己做一次。
//
// 為什麼放在 app 層而不是 engine/forms：FormDocument 的每一個回呼都在它
// 自己的專用執行緒上被呼叫（SDD §3.2），而這裡的批次流程需要在收到回呼後
// 建立/銷毀下一個 FormDocument——直接在該回呼裡做這件事會讓
// ~FormDocument() 的 thread.join() 對自己所在的執行緒自我 join，
// 結果是 std::terminate。engine 層不連結 Qt，沒有
// QMetaObject::invokeMethod 可用來把後續動作排回呼叫端執行緒；
// app 層可以，而且這正是 SDD §3.2「跨執行緒編組」明訂的責任分界。

#include <functional>
#include <string>
#include <vector>

#include "engine/forms/form_data.h"

namespace alioth::app {

enum class CsvMismatchPolicy {
    AbortBatch,  // 任何一列的欄位數與表頭不一致就整批中止，不再處理後續列
    SkipRow,     // 跳過該列，記錄原因，繼續處理其餘列
};

struct CsvFormFillRowResult {
    // 資料列序號，從 1 起算（不含表頭列），供使用者對照原始 CSV 的行號。
    std::int32_t rowNumber{0};
    bool ok{false};
    std::string outputPath;  // ok == true 時有效
    std::string error;       // ok == false 時必有原因（IL-4）
};

struct CsvFormFillReport {
    // 是否整批中止（AbortBatch 政策命中，或範本/CSV 本身無法使用）。
    // 中止時 rows 只包含中止之前已經處理過的列，不代表後面的列被判定失敗，
    // 而是完全沒有被嘗試——呼叫端必須把這個語意分清楚，不能都算進失敗列。
    bool aborted{false};
    std::string abortReason;

    // 表頭裡在範本找不到對應表單欄位的欄名。這些欄位在所有列都被忽略，
    // 不算進任一列的錯誤——它們是整份 CSV 的結構問題，不是某一列的問題。
    std::vector<std::string> ignoredColumns;

    std::vector<CsvFormFillRowResult> rows;

    [[nodiscard]] std::int32_t succeededCount() const noexcept;
    [[nodiscard]] std::int32_t failedCount() const noexcept;
};

struct CsvFormFillOptions {
    std::string templatePath;
    std::string password;  // 範本文件的開啟密碼；無密碼留空

    // 呼叫端負責讀檔內容（讀檔屬於平台層），這裡只負責解析。
    // 開頭的 UTF-8 BOM 由 domain::parseCsvDocument 自動去除。
    std::string csvText;

    std::string outputDirectory;

    // 輸出檔名樣板。{row} 會被替換成資料列序號（從 1 起算，補零至少 4 位）。
    // 刻意不支援以欄位值命名檔案：欄位值可能是空字串、可能含路徑不安全字元
    // （"/", "\\", ":" 在 Windows 上都是保留字元），序號永遠安全且不會重複。
    std::string outputNamePattern{"row_{row}.pdf"};

    CsvMismatchPolicy mismatchPolicy{CsvMismatchPolicy::SkipRow};
};

// 執行批次灌入。內部會在呼叫端所在的執行緒上驅動整個流程（每個 FormDocument
// 回呼都會先排回這條執行緒，見檔頭說明），因此呼叫端必須有一個正在跑的
// Qt 事件迴圈（GUI 執行緒或測試裡的 QCoreApplication 均可）。onDone 也在
// 這條執行緒上被呼叫。
void runCsvFormFill(const CsvFormFillOptions& options,
                    std::function<void(CsvFormFillReport)> onDone);

}  // namespace alioth::app
