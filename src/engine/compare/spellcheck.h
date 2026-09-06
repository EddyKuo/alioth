#pragma once

// 拼字檢查（PRD-SRCH-004：拼字檢查，作用於「註解與表單文字」）。
//
// **範圍與 find_replace.h 共用同一條邊界**：本產品不做內文文字編輯
// （PRD §2.1；PDFium 沒有文字重排能力），所以「文字層可編輯的情境」只有
// 註解內文（/Contents）與表單文字欄位值（/FT /Tx 的 /V）兩處，頁面內文
// 不在範圍內——理由與 find_replace.h 的檔頭完全一樣，這裡直接重用它的
// collectEditableText()，不重新走一次 /Annots、/AcroForm 的物件樹，
// 避免兩處各自維護「什麼算可編輯文字」而慢慢長歪（IL-3 單一真相來源）。
//
// **字典來源：BLOCKED，見 WP31 交付說明**。本專案的引擎級相依鎖定在三個元件
// （Qt 6 Widgets、PDFium、OpenSSL 3.x，CLAUDE.md），沒有一個內建拼字字典。
// Qt Widgets 本身不含拼字檢查（那是 KDE Sonnet 或作業系統服務，都不在目前的
// 相依清單內）。市面上可用的開源詞庫（Hunspell 系列 .dic/.aff、SCOWL 等）
// 本身是資料檔案不是「引擎級元件」，但仍需要一次明確的授權與語言範圍決策——
// 裝哪些語言、多大、算進 130 MB 安裝包上限的哪一塊——這不是 RD 可以自行拍板
// 的範圍（IL-1：資訊不完整時發出 BLOCKED，不得自行補全）。
//
// 因此這裡只內建 makeSampleDictionary()：幾百個常見英文詞與本產品自己的領域
// 詞彙，**僅供測試與展示管線可用，明確不是可上線的字典**。上線前需要一份 ADR
// 決定字典來源、授權與語言範圍，屆時只需要提供另一個 SpellDictionary 實作
// 換掉樣本字典即可——分詞、自訂字典、忽略清單、建議演算法都已經是完整實作，
// 換字典不需要動這些。

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/compare/find_replace.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::compare {

// 抽象字典介面，讓「字典從哪來」與「怎麼用字典」解耦——換成正式字典
// （或依語言切換）只需要提供另一個實作，不必動檢查邏輯。
class SpellDictionary {
public:
    virtual ~SpellDictionary() = default;
    [[nodiscard]] virtual bool contains(const std::string& lowerWord) const = 0;
};

// 記憶體字典：小寫詞的雜湊集合。詞一律先摺成 ASCII 小寫才存放與查詢——
// 拼字檢查目前只涵蓋 ASCII 字母組成的詞（見 tokenizeForSpelling 的說明），
// 摺大小寫的範圍限制與 utf8_scan.h::foldAscii 一致，理由相同：
// 完整 Unicode 大小寫摺疊需要 ICU，加不進三個引擎級元件的上限。
class InMemoryDictionary : public SpellDictionary {
public:
    explicit InMemoryDictionary(std::unordered_set<std::string> words);

    [[nodiscard]] bool contains(const std::string& lowerWord) const override;
    void addWord(const std::string& word);
    [[nodiscard]] std::size_t size() const noexcept { return words_.size(); }

private:
    std::unordered_set<std::string> words_;
};

// 見檔頭「字典來源」：這是樣本字典，不是可上線的字典。
[[nodiscard]] InMemoryDictionary makeSampleDictionary();

struct SpellToken {
    std::string text;  // 原始大小寫，UTF-8
    std::size_t byteOffset{0};
    std::size_t byteLength{0};
};

// 把一段文字切成可檢查的詞。只切出 ASCII 字母組成的詞（可含詞中的 ' 或 -，
// 例如 don't、co-worker，但兩端不含），純數字、CJK 表意文字、標點一律不切出來
// ——對沒有字典可查的內容做「拼字檢查」只會是一直誤判，比不檢查更糟，
// 因此刻意縮小範圍而不是「盡量猜」。
[[nodiscard]] std::vector<SpellToken> tokenizeForSpelling(const std::string& text);

struct SpellIssue {
    std::size_t targetIndex{0};  // 對應 collectEditableText() 的索引
    std::size_t byteOffset{0};
    std::size_t byteLength{0};
    std::string word;                     // 原始大小寫
    std::vector<std::string> suggestions;  // 編輯距離 1 的候選，依產生順序（刪→換位→替換→插入）
};

// 拼字檢查工作階段：字典 + 使用者自訂詞 + 忽略清單。三者都影響「算不算錯字」，
// 但只有字典會拿來產生建議——自訂詞與忽略清單只代表「不要再問我這個詞」，
// 不代表它是字典認得的正確拼法，因此不該被當成建議來源（把自訂詞當建議來源
// 會讓使用者把打錯的專有名詞越修越錯）。
//
// 自訂詞與忽略清單只存在記憶體裡；要跨工作階段保存是應用層的事
// （customWords()/ignoredWords() 已經是純字串集合，序列化很直接，
// 但那屬於 app 層的設定持久化範圍，不在本引擎轉接層職責內）。
class SpellCheckSession {
public:
    explicit SpellCheckSession(const SpellDictionary& dictionary);

    void addCustomWord(const std::string& word);
    void ignoreWord(const std::string& word);
    [[nodiscard]] const std::unordered_set<std::string>& customWords() const noexcept {
        return customWords_;
    }
    [[nodiscard]] const std::unordered_set<std::string>& ignoredWords() const noexcept {
        return ignoredWords_;
    }

    [[nodiscard]] bool isKnown(const std::string& word) const;

    // 編輯距離 1（刪除／相鄰換位／替換／插入各一次）的候選，只回傳字典裡真的有
    // 的字。這是 Peter Norvig 式的最小可行拼字校正，不需要額外相依。
    [[nodiscard]] std::vector<std::string> suggest(const std::string& word,
                                                    std::size_t maxSuggestions = 5) const;

    [[nodiscard]] std::vector<SpellIssue> checkText(const std::string& text) const;

    // 檢查整份文件的可編輯文字（註解內文與表單欄位值）。
    [[nodiscard]] std::vector<SpellIssue> checkDocument(
        const objects::PdfSourceDocument& source) const;

private:
    const SpellDictionary& dictionary_;
    std::unordered_set<std::string> customWords_;   // 已摺成小寫
    std::unordered_set<std::string> ignoredWords_;  // 已摺成小寫
};

}  // namespace alioth::engine::compare
