#include "engine/objects/accessibility_checker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

#include "engine/objects/reading_order.h"

namespace alioth::engine::objects {
namespace {

// ---- 小工具：只在本檔使用，刻意不動 struct_tree_reader.cpp 裡同名的私有版本 ----
// （兩邊都很短，重複比讓兩個本來就不同用途的模組互相依賴划算；
// struct_tree_reader 讀的是結構元素字典，這裡讀的是 Catalog／Info／Annots，
// 欄位重疊但語意不同鍵集合，硬併成一個共用函式反而會讓兩邊都要遷就對方。）

[[nodiscard]] std::string decodeTextStringLocal(const PdfString& string) {
    const std::string& bytes = string.bytes;
    std::string out;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const char32_t unit = static_cast<char32_t>((static_cast<unsigned char>(bytes[i]) << 8) |
                                                        static_cast<unsigned char>(bytes[i + 1]));
            // 只用於 ASCII 範圍的判定（有沒有標題文字），不需要處理代理對；
            // 完整 UTF-16→UTF-8 轉換見 struct_tree_reader.cpp 的版本。
            if (unit < 0x80) out.push_back(static_cast<char>(unit));
        }
        return out;
    }
    return bytes;
}

[[nodiscard]] std::string textEntryLocal(const PdfSourceDocument& source, const PdfDictionary& dict,
                                        const char* key) {
    const PdfObject* raw = dict.find(key);
    if (raw == nullptr) return {};
    const PdfObject resolved = source.resolve(*raw);
    if (const auto* string = std::get_if<PdfString>(&resolved.value())) {
        return decodeTextStringLocal(*string);
    }
    return {};
}

[[nodiscard]] const PdfDictionary* resolvedDict(const PdfSourceDocument& source,
                                                const PdfObject* raw, PdfObject& storage) {
    if (raw == nullptr) return nullptr;
    storage = source.resolve(*raw);
    return storage.asDictionary();
}

struct RgbColor {
    double r{0.0};
    double g{0.0};
    double b{0.0};
    bool valid{false};
};

[[nodiscard]] RgbColor colorFromNumberArray(const PdfArray& array) {
    std::vector<double> nums;
    nums.reserve(array.size());
    for (const PdfObject& item : array) {
        if (item.isNumber()) nums.push_back(item.asNumber());
    }
    RgbColor color;
    if (nums.size() == 1) {
        color = {nums[0], nums[0], nums[0], true};
    } else if (nums.size() == 3) {
        color = {nums[0], nums[1], nums[2], true};
    } else if (nums.size() == 4) {
        // CMYK 的簡化換算（ISO 32000-1 §8.6.5.3），足供對比估算，不追求印刷精確。
        const double k = nums[3];
        color = {(1.0 - nums[0]) * (1.0 - k), (1.0 - nums[1]) * (1.0 - k),
                 (1.0 - nums[2]) * (1.0 - k), true};
    }
    return color;
}

// 解析 /DA（Default Appearance）字串裡最後一次出現的顏色運算子。
// /DA 是一串內容運算子（例如 "0 0 0 rg /Helv 12 Tf"），不是結構化資料，
// 因此用簡單的逆波蘭式棧來還原：遇到數字就推進緩衝，遇到認得的顏色運算子
// 就用緩衝尾端對應個數的數字組色並清空緩衝；其餘 token（字型名稱、Tf 等）
// 一律清空緩衝，避免誤把字級或無關數字當成顏色分量。
[[nodiscard]] RgbColor colorFromDefaultAppearance(const std::string& da) {
    RgbColor result;
    std::vector<double> nums;
    std::istringstream stream(da);
    std::string token;
    while (stream >> token) {
        if (token == "g" && !nums.empty()) {
            const double v = nums.back();
            result = {v, v, v, true};
            nums.clear();
        } else if (token == "rg" && nums.size() >= 3) {
            const std::size_t n = nums.size();
            result = {nums[n - 3], nums[n - 2], nums[n - 1], true};
            nums.clear();
        } else if (token == "k" && nums.size() >= 4) {
            const std::size_t n = nums.size();
            const double c = nums[n - 4];
            const double m = nums[n - 3];
            const double y = nums[n - 2];
            const double kk = nums[n - 1];
            result = {(1.0 - c) * (1.0 - kk), (1.0 - m) * (1.0 - kk), (1.0 - y) * (1.0 - kk), true};
            nums.clear();
        } else {
            char* end = nullptr;
            const double value = std::strtod(token.c_str(), &end);
            if (end != token.c_str() && *end == '\0') {
                nums.push_back(value);
            } else {
                nums.clear();
            }
        }
    }
    return result;
}

[[nodiscard]] double linearizeChannel(double channel) {
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double relativeLuminanceOf(const RgbColor& color) {
    return 0.2126 * linearizeChannel(color.r) + 0.7152 * linearizeChannel(color.g) +
           0.0722 * linearizeChannel(color.b);
}

[[nodiscard]] double contrastRatioOf(const RgbColor& a, const RgbColor& b) {
    const double la = relativeLuminanceOf(a);
    const double lb = relativeLuminanceOf(b);
    const double lighter = std::max(la, lb);
    const double darker = std::min(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

constexpr double kContrastAaNormal = 4.5;

[[nodiscard]] bool isHeadingLevel(const std::string& type, int& level) {
    if (type.size() == 2 && type[0] == 'H' && type[1] >= '1' && type[1] <= '9') {
        level = type[1] - '0';
        return true;
    }
    return false;
}

// ---- 檢查一：文件標題 ----
void checkDocumentTitle(const PdfSourceDocument& source, std::vector<A11yFinding>& findings) {
    std::string title;
    if (const PdfObject* infoRaw = source.trailer().find("Info")) {
        PdfObject infoStorage;
        if (const PdfDictionary* info = resolvedDict(source, infoRaw, infoStorage)) {
            title = textEntryLocal(source, *info, "Title");
        }
    }
    bool displayDocTitle = false;
    PdfObject catalogStorage;
    const PdfDictionary* catalog =
        resolvedDict(source, source.trailer().find("Root"), catalogStorage);
    if (catalog != nullptr) {
        PdfObject prefsStorage;
        if (const PdfDictionary* prefs =
                resolvedDict(source, catalog->find("ViewerPreferences"), prefsStorage)) {
            if (const PdfObject* flag = prefs->find("DisplayDocTitle")) {
                const PdfObject resolved = source.resolve(*flag);
                if (const auto* b = std::get_if<bool>(&resolved.value())) displayDocTitle = *b;
            }
        }
    }

    if (title.empty()) {
        findings.push_back(A11yFinding{
            A11yCheckId::DocumentTitle, -1, "Document /Info /Title",
            "文件沒有標題中繼資料；螢幕閱讀器與工作列會改用檔案名稱，"
            "使用者無法從朗讀內容判斷這是哪一份文件",
            "在文件屬性對話框填入有意義的標題（不是檔案名稱），並確認 "
            "/ViewerPreferences /DisplayDocTitle 設為 true 讓檢視器顯示它而不是檔名"});
    } else if (!displayDocTitle) {
        findings.push_back(A11yFinding{
            A11yCheckId::DocumentTitle, -1, "Catalog /ViewerPreferences /DisplayDocTitle",
            "文件有標題，但 /DisplayDocTitle 未設為 true，檢視器視窗標題仍會顯示檔名",
            "在文件屬性對話框勾選「以文件標題取代檔名」"});
    }
}

// ---- 檢查二：文件語言 ----
void checkDocumentLanguage(const PdfSourceDocument& source, std::vector<A11yFinding>& findings) {
    PdfObject catalogStorage;
    const PdfDictionary* catalog =
        resolvedDict(source, source.trailer().find("Root"), catalogStorage);
    const std::string lang = catalog != nullptr ? textEntryLocal(source, *catalog, "Lang") : "";
    if (lang.empty()) {
        findings.push_back(A11yFinding{
            A11yCheckId::DocumentLanguage, -1, "Catalog /Lang",
            "文件沒有設定語言，螢幕閱讀器無法自動選擇正確的發音規則（例如中文與英文的斷詞、"
            "重音完全不同）",
            "在文件屬性設定主要語言（例如 zh-TW），必要時個別文字區塊可用 /Lang 覆蓋"});
    }
}

// ---- 檢查三：標籤結構 ----
void checkTagStructure(const StructTree& tree, std::vector<A11yFinding>& findings) {
    if (tree.status == StructTreeStatus::Ok || tree.status == StructTreeStatus::Truncated) {
        if (!tree.markedContent) {
            findings.push_back(A11yFinding{
                A11yCheckId::TagStructure, -1, "Catalog /MarkInfo /Marked",
                "有結構樹，但 /MarkInfo /Marked 不是 true，代表內容標記未完整完成，"
                "閱讀順序不保證正確",
                "重新以標籤化匯出流程處理整份文件，確認匯出後 /MarkInfo /Marked 為 true"});
        }
        return;
    }
    findings.push_back(A11yFinding{
        A11yCheckId::TagStructure, -1, "Catalog /StructTreeRoot",
        std::string("文件沒有可用的標籤結構（") + describe(tree.status) +
            "）：螢幕閱讀器無法判斷閱讀順序，圖片沒有替代文字入口，表格讀不出列首欄首",
        "以支援標籤化輸出的工具重新產生 PDF（例如從原始排版軟體匯出時開啟 "
        "「保留輔助功能標籤」），或使用標籤化工具替既有文件補上結構"});
}

// ---- 檢查四：替代文字（沿用 struct_tree_reader 既有的 needsAlternateText 判定）----
void checkAltText(const std::vector<const StructNode*>& flat, std::vector<A11yFinding>& findings) {
    for (const StructNode* node : flat) {
        if (node->needsAlternateText() && !node->hasAlternateText()) {
            std::string element = node->type;
            if (node->objectNumber != 0) element += " (物件 " + std::to_string(node->objectNumber) + ")";
            findings.push_back(A11yFinding{
                A11yCheckId::ImageAltText, node->pageIndex, element,
                "這個" + node->type + " 元素沒有 /Alt 也沒有 /ActualText，螢幕閱讀器只會念出角色"
                "名稱（例如「圖形」）而不會念出內容說明",
                "在 Tags 面板為這個元素填寫簡短、描述性的替代文字（PRD-A11Y-004）"});
        }
    }
}

// ---- 檢查五：表格表頭 ----
[[nodiscard]] bool hasDescendantOfType(const StructNode& node, const std::string& type) {
    for (const StructNode& child : node.children) {
        if (child.type == type || hasDescendantOfType(child, type)) return true;
    }
    return false;
}

void checkTableHeaders(const std::vector<const StructNode*>& flat,
                       std::vector<A11yFinding>& findings) {
    for (const StructNode* node : flat) {
        if (node->type != "Table") continue;
        if (!hasDescendantOfType(*node, "TH")) {
            std::string element = "Table";
            if (node->objectNumber != 0) element += " (物件 " + std::to_string(node->objectNumber) + ")";
            findings.push_back(A11yFinding{
                A11yCheckId::TableHeaders, node->pageIndex, element,
                "這個表格底下沒有任何 /TH 儲存格，螢幕閱讀器逐格朗讀時聽不出這是第幾欄第幾列",
                "把表格首列（或首欄）的儲存格結構型別改成 /TH，並視需要加上 /Scope"});
        }
    }
}

// ---- 檢查六：標題層級跳級 ----
void checkHeadingHierarchy(const std::vector<const StructNode*>& flat,
                           std::vector<A11yFinding>& findings) {
    int previousLevel = 0;
    for (const StructNode* node : flat) {
        int level = 0;
        if (!isHeadingLevel(node->type, level)) continue;
        if (previousLevel == 0 && level != 1) {
            findings.push_back(A11yFinding{
                A11yCheckId::HeadingHierarchy, node->pageIndex, node->type,
                "文件的第一個標題不是 H1，標題階層從中間層級開始",
                "把文件最外層的標題改為 H1，讓標題階層從最高層開始"});
        } else if (previousLevel != 0 && level > previousLevel + 1) {
            findings.push_back(A11yFinding{
                A11yCheckId::HeadingHierarchy, node->pageIndex, node->type,
                "標題層級從 H" + std::to_string(previousLevel) + " 跳到 H" +
                    std::to_string(level) + "，中間跳過的層級會讓螢幕閱讀器使用者以為漏看了一段",
                "補上中間層級的標題，或把這個標題改成正確的層級"});
        }
        previousLevel = level;
    }
}

// ---- 檢查七：閱讀順序 vs 內容順序 ----
void checkReadingOrder(const StructTree& tree, std::vector<A11yFinding>& findings) {
    std::set<std::int32_t> pages;
    for (const StructNode* node : flatten(tree)) {
        if (node->pageIndex >= 0) pages.insert(node->pageIndex);
    }
    for (const std::int32_t page : pages) {
        const PageReadingOrder order = computePageReadingOrder(tree, page);
        if (order.mismatchIndices.empty()) continue;
        findings.push_back(A11yFinding{
            A11yCheckId::ReadingOrder, page,
            std::to_string(order.mismatchIndices.size()) + " 個結構元素",
            "這一頁的結構順序與內容（MCID）輸出順序不一致：螢幕閱讀器念出的段落次序，"
            "與內容實際被畫出來的次序不同，常見於多欄版面或事後手動調整過標籤順序",
            "在 Order 面板檢視這一頁的結構順序，把跳號的元素拖曳到符合視覺閱讀方向的位置"});
    }
}

// ---- 檢查八：對比（僅 FreeText 註解的 /DA 文字色 vs /C 背景色）----
void checkAnnotationContrast(const PdfSourceDocument& source, std::vector<A11yFinding>& findings) {
    for (std::size_t pageIdx = 0; pageIdx < source.pages().size(); ++pageIdx) {
        const PdfRef& pageRef = source.pages()[pageIdx];
        const PdfObject pageObject = source.object(pageRef.number);
        const PdfDictionary* pageDict = pageObject.asDictionary();
        if (pageDict == nullptr) continue;

        PdfObject annotsStorage;
        const PdfObject* annotsRaw = pageDict->find("Annots");
        if (annotsRaw == nullptr) continue;
        annotsStorage = source.resolve(*annotsRaw);
        const PdfArray* annots = annotsStorage.asArray();
        if (annots == nullptr) continue;

        for (const PdfObject& entry : *annots) {
            const PdfObject annotObject = source.resolve(entry);
            const PdfDictionary* annot = annotObject.asDictionary();
            if (annot == nullptr) continue;
            if (const PdfObject* subtype = annot->find("Subtype");
                subtype == nullptr || source.resolve(*subtype).asName() != "FreeText") {
                continue;
            }

            RgbColor background;
            if (const PdfObject* colorEntry = annot->find("C")) {
                const PdfObject resolved = source.resolve(*colorEntry);
                if (const PdfArray* array = resolved.asArray()) background = colorFromNumberArray(*array);
            }
            RgbColor text;
            if (const PdfObject* daEntry = annot->find("DA")) {
                const PdfObject resolved = source.resolve(*daEntry);
                if (const auto* string = std::get_if<PdfString>(&resolved.value())) {
                    text = colorFromDefaultAppearance(decodeTextStringLocal(*string));
                }
            }
            if (!background.valid || !text.valid) continue;  // 量不到，不猜測

            const double ratio = contrastRatioOf(text, background);
            if (ratio >= kContrastAaNormal) continue;

            std::string element = "FreeText";
            if (entry.isRef()) element += " (物件 " + std::to_string(entry.asRef().number) + ")";
            findings.push_back(A11yFinding{
                A11yCheckId::Contrast, static_cast<std::int32_t>(pageIdx), element,
                "這則文字註解的文字顏色與背景顏色對比為 " +
                    std::to_string(static_cast<int>(ratio * 100) / 100.0) +
                    ":1，未達 WCAG AA 一般文字要求的 4.5:1",
                "在註解屬性面板調整文字顏色或背景色，提高對比後再儲存"});
        }
    }
}

}  // namespace

const char* describe(A11yCheckId id) noexcept {
    switch (id) {
        case A11yCheckId::DocumentTitle:    return "文件標題";
        case A11yCheckId::DocumentLanguage: return "文件語言";
        case A11yCheckId::TagStructure:     return "標籤結構";
        case A11yCheckId::ImageAltText:     return "替代文字";
        case A11yCheckId::TableHeaders:     return "表格表頭";
        case A11yCheckId::HeadingHierarchy: return "標題層級";
        case A11yCheckId::ReadingOrder:     return "閱讀順序";
        case A11yCheckId::Contrast:         return "對比";
    }
    return "未知";
}

A11yReport runAccessibilityCheck(const PdfSourceDocument& source, const StructTree& tree) {
    A11yReport report;

    checkDocumentTitle(source, report.findings);
    checkDocumentLanguage(source, report.findings);
    checkTagStructure(tree, report.findings);

    if (tree.status == StructTreeStatus::Ok || tree.status == StructTreeStatus::Truncated) {
        const std::vector<const StructNode*> flat = flatten(tree);
        checkAltText(flat, report.findings);
        checkTableHeaders(flat, report.findings);
        checkHeadingHierarchy(flat, report.findings);
        checkReadingOrder(tree, report.findings);
    }

    checkAnnotationContrast(source, report.findings);

    report.coverage = {
        {A11yCheckId::DocumentTitle, true,
         "檢查標題是否存在與是否設定顯示；不判斷標題內容是否有意義"},
        {A11yCheckId::DocumentLanguage, true,
         "只檢查 Catalog /Lang 是否存在；不檢查語言碼是否正確描述實際內容語言"},
        {A11yCheckId::TagStructure, true, "涵蓋有無結構樹與 /MarkInfo /Marked"},
        {A11yCheckId::ImageAltText, true,
         "只涵蓋標準型別 Figure/Formula/Form/Link；透過 /RoleMap 對應到這些型別的自訂型別"
         "目前不會被展開判定，會被目前實作視為一般型別而略過"},
        {A11yCheckId::TableHeaders, true,
         "只檢查表格底下是否存在至少一個 /TH；不檢查 /TH 的 /Scope 或儲存格與表頭的 "
         "/Headers 屬性關聯是否正確"},
        {A11yCheckId::HeadingHierarchy, true,
         "只檢查 H1–H9 數字化層級的跳級與起始層級；/H（無層級）與屬性字典裡的自訂 /Lvl 不判定"},
        {A11yCheckId::ReadingOrder, true,
         "以 MCID（內容標記順序）做為內容順序的代理指標，不是幾何視覺順序；"
         "沒有 MCID 的結構元素不計入比對，也不會顯示為缺陷（見 reading_order.h 的說明）"},
        {A11yCheckId::Contrast, true,
         "只涵蓋 FreeText 註解，且僅在該註解同時有 /C（背景色）與 /DA（文字色）時才能量測；"
         "一般內文文字、影像內文字、向量圖形的顏色需要逐字元追蹤內容串流色彩狀態，"
         "本輪未實作，不在這裡報告，也不會被誤判為「已檢查、沒問題」"},
    };

    report.notCoveredAtAll = {
        "一般內文文字與影像內文字的對比（WCAG 1.4.3）：需要逐字元追蹤內容串流的色彩狀態"
        "（rg/g/k/scn 等運算子交錯出現在文字與圖形之間），是一套獨立的內容串流色彩追蹤子系統，"
        "本輪未實作",
        "顏色是否為唯一的語意辨識方式（WCAG 1.4.1，例如只用紅字表示錯誤而不加文字或圖示）："
        "需要理解內容的語意用途，物件層或結構樹本身無法判定",
        "表單欄位的標籤關聯（/TU 是否存在、Widget 與說明文字的 /StructParent 對應是否正確）："
        "屬於 PRD WP6 表單子系統的範圍，本輪的檢查器沒有涵蓋",
        "多媒體字幕與轉錄稿：本產品的排除範圍（PRD §2.1 不含多媒體嵌入編輯），因此也沒有對應檢查",
        "XFA 表單的無障礙結構：專案立場是永不啟用 XFA（PRD §8.2 安全立場），不會為它另建檢查路徑",
    };

    return report;
}

}  // namespace alioth::engine::objects
