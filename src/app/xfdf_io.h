#pragma once

// XFDF 匯入／匯出(PRD-ANN-013)。
//
// XFDF(ISO 19444-1)是 Acrobat 用來交換註解的 XML 格式,不含頁面內容本身,
// 只有註解列表——這正是「匯出選定註解」語意上該用的格式:使用者只想把
// 挑出來的幾則意見交給別人,不是整份文件。
//
// 安全立場(CLAUDE.md「PDF 視為不可信任輸入」同樣適用於這裡,XFDF 來自
// 使用者收到的信件附件,信任程度不會比 PDF 本身更高):
//   - 拒絕任何帶 <!DOCTYPE 的輸入。這一道防線同時擋掉 billion-laughs
//     (靠 DOCTYPE 內部定義的實體遞迴展開)與外部實體注入(SYSTEM/PUBLIC
//     實體讀取本機檔案或發出網路請求)——XFDF 規格本身不需要 DOCTYPE,
//     禁掉它不會犧牲任何合法功能。
//   - 輸入大小有上限,避免單純的巨大檔案就先耗盡記憶體。
//
// 只支援審閱最常見的子集:文字標記四種、Square、Circle、Line、Ink、
// Text(便利貼)、FreeText(Text Box)。PRD 未要求的表單欄位、書籤、
// 附件、圖層在這裡不處理，遇到不支援的元素一律跳過並在診斷中列出，
// 不是靜默漏收。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/annotation.h"

namespace alioth::app {

// 一則要匯出/匯入的註解，外加它所在的頁碼(0 起算)——XFDF 的 <annots>
// 底下每個元素都自帶 page 屬性，不是整批共用一個頁碼。
struct XfdfEntry {
    std::int32_t pageIndex{0};
    domain::Annotation annotation;
};

// 把一批註解序列化成 XFDF 位元組(UTF-8)。sourceFilename 對應 <f href="...">，
// 純粹是給收件人辨識用的提示，不影響匯入端的行為，留空即可省略該欄位。
[[nodiscard]] std::string exportXfdf(const std::vector<XfdfEntry>& entries,
                                     const std::string& sourceFilename = {});

struct XfdfImportResult {
    bool ok{false};
    std::string diagnostic;               // 失敗原因,或部分跳過項目的說明
    std::vector<XfdfEntry> entries;
    std::vector<std::string> skippedElements;  // 辨識但不支援/解析失敗的元素名稱
};

// 解析 XFDF 位元組。任何 DOCTYPE 宣告一律先被拒絕，不進入 XML 剖析器。
[[nodiscard]] XfdfImportResult importXfdf(const std::string& xml);

}  // namespace alioth::app
