#include "engine/forms/form_types.h"

namespace alioth::engine::forms {

const char* describe(FormFieldType type) noexcept {
    switch (type) {
        case FormFieldType::PushButton:  return "按鈕";
        case FormFieldType::CheckBox:    return "核取方塊";
        case FormFieldType::RadioButton: return "單選按鈕";
        case FormFieldType::ComboBox:    return "下拉方塊";
        case FormFieldType::ListBox:     return "清單方塊";
        case FormFieldType::TextField:   return "文字欄位";
        case FormFieldType::Signature:   return "簽章欄位";
        case FormFieldType::Xfa:         return "XFA 欄位";
        case FormFieldType::Unknown:     break;
    }
    return "未知欄位";
}

const char* describe(FormType type) noexcept {
    switch (type) {
        case FormType::AcroForm:      return "AcroForm";
        case FormType::XfaFull:       return "動態 XFA";
        case FormType::XfaForeground: return "XFA 疊加（XFAF）";
        case FormType::None:          break;
    }
    return "無表單";
}

XfaReport describeXfa(FormType type, bool acroFormFieldsExist) {
    XfaReport report;
    report.present = (type == FormType::XfaFull || type == FormType::XfaForeground);
    if (!report.present) return report;

    report.dynamic = (type == FormType::XfaFull);
    report.acroFormFallbackUsable = acroFormFieldsExist;

    // 訊息分成三種而不是一種：使用者要能從提示判斷「能不能繼續作業」。
    // 只寫「不支援 XFA」會讓有可用後備欄位的文件被誤判為不能填。
    if (report.dynamic && !acroFormFieldsExist) {
        report.message =
            "本文件為動態 XFA 表單，版面完全由 XFA 描述，且未附可用的 AcroForm 後備內容。"
            "目前顯示的是文件內建的靜態後備頁面，欄位無法填寫；"
            "如需填寫請改用文件提供者指定的 XFA 相容工具。";
    } else if (report.dynamic) {
        report.message =
            "本文件為動態 XFA 表單。目前顯示與可填寫的是 AcroForm 後備欄位，"
            "XFA 定義的動態版面、計算與驗證規則不會生效，"
            "填寫結果可能與文件提供者預期不同。";
    } else {
        report.message =
            "本文件含 XFA 疊加內容（XFAF）。AcroForm 欄位可正常填寫，"
            "但 XFA 額外定義的版面與規則不會生效。";
    }
    return report;
}

}  // namespace alioth::engine::forms
