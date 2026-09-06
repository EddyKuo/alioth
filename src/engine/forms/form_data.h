#pragma once

// 表單資料匯入匯出（WBS 6.3，PRD-FORM-002）。
//
// 這一層刻意做成純函數：輸入欄位快照、輸出位元組；輸入位元組、輸出欄位值。
// 完全不碰 PDFium，也不碰執行緒。理由與外觀串流產生器相同——
// FDF / XFDF 的語法細節（跳脫、UTF-16、實體參照）值得逐字元驗證，
// 而那種測試不該需要開啟一份 PDF。
//
// 匯出範圍遵循 PDF 規格：/Ff 的 NoExport 欄位與按鈕型欄位不寫出。
// 簽章欄位也不寫出——把簽章值搬到另一份文件沒有意義，而且會誤導。

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/forms/form_types.h"

namespace alioth::engine::forms {

enum class FormDataFormat {
    Fdf,   // ISO 32000-2 §12.7.8，PDF 物件語法
    Xfdf,  // XFDF 3.0，XML 語法
};

// 匯入結果的一筆。多值只有清單方塊（複選）會出現。
struct FormDataEntry {
    std::string name;
    std::vector<std::string> values;

    [[nodiscard]] const std::string& primary() const;
};

struct FormDataImport {
    bool ok{false};
    std::string error;  // 繁體中文，可直接顯示
    std::vector<FormDataEntry> entries;
};

// sourcePath 會寫進 /F 或 <f href>，供 PDF 閱讀器回頭找原始文件。
// 允許為空字串（匿名匯出）。
[[nodiscard]] std::string exportFormData(const std::vector<FormFieldInfo>& fields,
                                         FormDataFormat format,
                                         std::string_view sourcePath = {});

[[nodiscard]] FormDataImport importFormData(std::string_view text, FormDataFormat format);

// 依內容推測格式。XFDF 一定以 XML 宣告或 <xfdf 開頭，FDF 一定以 %FDF 開頭；
// 兩者都不像時回傳 nullopt，而不是猜一個——猜錯會產生一堆看似成功的空欄位。
[[nodiscard]] std::optional<FormDataFormat> detectFormat(std::string_view text);

// 欄位是否應該被寫進匯出檔。公開出來讓呼叫端能事先算出「將匯出 N 個欄位」。
[[nodiscard]] bool isExportable(const FormFieldInfo& field);

}  // namespace alioth::engine::forms
