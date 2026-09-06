#include "engine/formbuild/field_definition.h"

#include <algorithm>

#include "domain/barcode.h"

namespace alioth::engine::formbuild {

namespace {

// /Ff 的位元編號（ISO 32000-2 §12.7.4 表 227 / 228 / 230）。
// 以編號而非最終值命名，讓每一條都能直接對回規格表；
// 直接寫 131072 這種常數在 code review 時沒有人能驗證它是對的。
constexpr std::int64_t bit(int number) { return std::int64_t{1} << (number - 1); }

constexpr std::int64_t kReadOnly = bit(1);
constexpr std::int64_t kRequired = bit(2);
constexpr std::int64_t kNoExport = bit(3);
constexpr std::int64_t kMultiline = bit(13);
constexpr std::int64_t kPassword = bit(14);
constexpr std::int64_t kNoToggleToOff = bit(15);
constexpr std::int64_t kRadio = bit(16);
constexpr std::int64_t kPushbutton = bit(17);
constexpr std::int64_t kCombo = bit(18);
constexpr std::int64_t kEdit = bit(19);
constexpr std::int64_t kSort = bit(20);
constexpr std::int64_t kMultiSelect = bit(22);
constexpr std::int64_t kDoNotSpellCheck = bit(23);
constexpr std::int64_t kComb = bit(25);

// PDF 名稱物件不能含空白與分隔字元。匯出值會被寫成 /AP /N 的鍵與 /AS 的值，
// 兩處都是名稱；含空白的匯出值產出的檔案在 Acrobat 會直接解析失敗。
[[nodiscard]] bool isSafeNameToken(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c > 0x20 && c < 0x7F && c != '/' && c != '%' && c != '(' && c != ')' &&
               c != '<' && c != '>' && c != '[' && c != ']' && c != '{' && c != '}' && c != '#';
    });
}

}  // namespace

const char* describe(BuildFieldType type) noexcept {
    switch (type) {
        case BuildFieldType::Text: return "文字欄位";
        case BuildFieldType::CheckBox: return "核取方塊";
        case BuildFieldType::RadioGroup: return "單選按鈕群組";
        case BuildFieldType::ComboBox: return "下拉方塊";
        case BuildFieldType::ListBox: return "清單方塊";
        case BuildFieldType::PushButton: return "按鈕";
        case BuildFieldType::Date: return "日期欄位";
        case BuildFieldType::Image: return "影像欄位";
        case BuildFieldType::Signature: return "數位簽章欄位";
        case BuildFieldType::Barcode: return "條碼欄位";
    }
    return "未知欄位";
}

const char* fieldTypeName(BuildFieldType type) noexcept {
    switch (type) {
        case BuildFieldType::Text:
        case BuildFieldType::Date:
        case BuildFieldType::Barcode:
            return "Tx";
        case BuildFieldType::CheckBox:
        case BuildFieldType::RadioGroup:
        case BuildFieldType::PushButton:
        case BuildFieldType::Image:
            return "Btn";
        case BuildFieldType::ComboBox:
        case BuildFieldType::ListBox:
            return "Ch";
        case BuildFieldType::Signature:
            return "Sig";
    }
    return "Tx";
}

std::int64_t computeFieldFlags(const FieldDefinition& definition) {
    std::int64_t flags = 0;
    if (definition.readOnly) flags |= kReadOnly;
    if (definition.required) flags |= kRequired;
    if (definition.noExport) flags |= kNoExport;

    switch (definition.type) {
        case BuildFieldType::Text:
            if (definition.multiline) flags |= kMultiline;
            if (definition.password) flags |= kPassword;
            // /Comb 只在同時有 /MaxLen 且非多行、非密碼時才成立（§12.7.4.3）。
            // 少了這個條件，Acrobat 會忽略整個旗標而不是報錯，
            // 於是「格線輸入框」變成普通輸入框，沒有人知道為什麼。
            if (definition.comb && definition.maxLength > 0 && !definition.multiline &&
                !definition.password) {
                flags |= kComb;
            }
            break;

        case BuildFieldType::Date:
            // 日期不該被拼字檢查標紅線。
            flags |= kDoNotSpellCheck;
            break;

        case BuildFieldType::CheckBox:
            break;

        case BuildFieldType::RadioGroup:
            // NoToggleToOff：已選中的按鈕再按一次不會取消。這是單選群組的
            // 標準行為，少了它使用者可以把整組都取消，違反「單選」的語意。
            flags |= kRadio | kNoToggleToOff;
            break;

        case BuildFieldType::PushButton:
        case BuildFieldType::Image:
            // 按鈕沒有值，因此一律不匯出；不設這個旗標會讓 FDF 出現空鍵。
            flags |= kPushbutton | kNoExport;
            break;

        case BuildFieldType::ComboBox:
            flags |= kCombo;
            if (definition.comboEditable) flags |= kEdit;
            if (definition.sortOptions) flags |= kSort;
            break;

        case BuildFieldType::ListBox:
            if (definition.multiSelect) flags |= kMultiSelect;
            if (definition.sortOptions) flags |= kSort;
            break;

        case BuildFieldType::Signature:
            break;

        case BuildFieldType::Barcode:
            // 條碼欄位一律唯讀：外觀是依照 /V 的值計算出來的條碼圖形，
            // 若允許使用者直接編輯畫面上的文字，畫面與 /V 會立刻不同步。
            // 要改內容得改 /V（例如透過表單計算或程式化填寫），不是讓人
            // 在條碼上打字。
            flags |= kReadOnly;
            break;
    }
    return flags;
}

std::string validate(const FieldDefinition& definition) {
    if (definition.name.empty()) return "欄位名稱不可為空";
    // 欄位名以點分層，因此名稱本身不能有點開頭或連續點，否則樹會出現空節點。
    if (definition.name.front() == '.' || definition.name.back() == '.') {
        return "欄位名稱不可以點開頭或結尾：" + definition.name;
    }
    if (definition.name.find("..") != std::string::npos) {
        return "欄位名稱不可含連續的點：" + definition.name;
    }
    if (definition.pageIndex < 0) return "頁碼不可為負數";

    if (definition.type == BuildFieldType::RadioGroup) {
        if (definition.radios.size() < 2) {
            // 只有一顆的單選群組在 UI 上永遠無法取消選取，是使用者陷阱而不是功能。
            return "單選群組至少需要兩顆按鈕：" + definition.name;
        }
        std::vector<std::string> seen;
        for (const RadioOption& option : definition.radios) {
            if (!isSafeNameToken(option.exportValue)) {
                return "單選按鈕的匯出值必須是合法的 PDF 名稱：" + option.exportValue;
            }
            if (option.exportValue == "Off") {
                // /Off 是「未選取」的保留狀態名，拿它當某顆按鈕的值
                // 會讓那顆按鈕永遠顯示成未選取。
                return "單選按鈕的匯出值不可為 Off";
            }
            if (std::find(seen.begin(), seen.end(), option.exportValue) != seen.end()) {
                return "單選按鈕的匯出值重複：" + option.exportValue;
            }
            seen.push_back(option.exportValue);
            if (option.rectPt.normalized().isEmpty()) {
                return "單選按鈕的矩形不可為空：" + option.exportValue;
            }
        }
        return {};
    }

    if (definition.rectPt.normalized().isEmpty()) {
        return "欄位矩形的寬或高為零，欄位會看不見也點不到：" + definition.name;
    }

    if (definition.type == BuildFieldType::CheckBox) {
        if (!isSafeNameToken(definition.exportValue)) {
            return "核取方塊的匯出值必須是合法的 PDF 名稱：" + definition.exportValue;
        }
        if (definition.exportValue == "Off") return "核取方塊的匯出值不可為 Off";
    }

    if (definition.type == BuildFieldType::ComboBox ||
        definition.type == BuildFieldType::ListBox) {
        if (definition.options.empty()) {
            return "選項清單不可為空：" + definition.name;
        }
    }

    if (definition.type == BuildFieldType::Barcode) {
        if (!domain::barcode::isSubsetBText(definition.value)) {
            return "條碼欄位的值必須是非空的可列印 ASCII（Code 128 Subset B）：" + definition.name;
        }
    }

    if (definition.fontSize < 0.0) return "字級不可為負數";
    if (definition.maxLength < 0) return "最大長度不可為負數";
    return {};
}

namespace {

[[nodiscard]] domain::RectF offsetRect(const domain::RectF& rect, double dx, double dy) {
    return domain::RectF{rect.left + dx, rect.bottom + dy, rect.right + dx, rect.top + dy};
}

}  // namespace

FieldDefinition duplicateFieldDefinition(const FieldDefinition& source, std::string newName,
                                         double offsetXPt, double offsetYPt) {
    FieldDefinition copy = source;
    copy.name = std::move(newName);

    if (copy.type == BuildFieldType::RadioGroup) {
        for (RadioOption& option : copy.radios) {
            option.rectPt = offsetRect(option.rectPt, offsetXPt, offsetYPt);
        }
    } else {
        copy.rectPt = offsetRect(copy.rectPt, offsetXPt, offsetYPt);
    }
    return copy;
}

}  // namespace alioth::engine::formbuild
