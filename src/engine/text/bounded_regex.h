#pragma once

// 保證線性時間的正規表示式引擎（PRD-SRCH-002：正規表示式搜尋）。
//
// **為什麼不用 <regex>**：std::regex（以及幾乎所有主流語言內建的正則引擎）
// 是回溯（backtracking）實作，最壞情況時間對某些樣式是輸入長度的指數函數
// ——這就是「災難性回溯」。查詢字串是使用者輸入，PDF 視為不可信任輸入的
// 同一個原則在這裡同樣適用：不能讓使用者（或惡意輸入）打一個樣式就讓搜尋
// 執行緒卡死，尤其它跑在文字執行緒上會擋住同一份文件之後所有的搜尋與選取。
//
// 這裡的解法不是加逾時把回溯引擎腰斬（那治標不治本：腰斬前那幾百毫秒仍然是
// 真的卡死，而且無法安全地從回溯引擎的呼叫堆疊中途抽身），而是換一種
// **不會回溯**的演算法：Thompson NFA 構造 + Pike VM 式的同步模擬
// （所有可能的執行緒逐字元一起前進，而不是逐一嘗試再回頭重來）。
// 這保證了時間複雜度是 O(text.size() * NFA 狀態數)，與輸入內容本身無關，
// 是子字串搜尋演算法（如 Aho-Corasick、KMP）在正則表示式上的類比。
//
// **付出的代價**：不支援回溯引擎才能表達的功能——回頭參照（backreference，
// 例如 \1）與環視斷言（lookahead/lookbehind，例如 (?=...)）。這兩者在數學上
// 需要非正規語言的表達力，不存在不回溯的通用演算法（RE2、Go 的 regexp、
// Rust regex crate都是同樣的取捨，理由相同）。編譯時遇到這些語法會回報
// diagnostic 而不是假裝支援。非貪婪量詞（*?、+?、??）也不支援：
// 本引擎只找「是否命中」與命中範圍，不需要跟回溯引擎一樣區分貪婪／非貪婪
// 兩種語意下的候選命中順序。
//
// 支援的子集：字面字元、.（任意碼點）、字元類 [abc] [^abc] [a-z]、
// 常見跳脫類 \d \D \w \W \s \S、量詞 * + ? {m,n} {m,}、交替 |、
// 群組 ( )（僅供優先序分組，不做捕獲——本引擎的用途是「找出命中範圍」，
// 不是取子群，捕獲會讓 VM 從無狀態的並行模擬變成需要每條執行緒攜帶額外
// 記錄，複雜度不成比例）、行首 / 行尾錨點 ^ $（對應整段輸入的開頭／結尾，
// 每次搜尋的輸入是單頁文字，不是多行文件，因此不需要多行模式）。
//
// 運算單位是 Unicode 碼點而非位元組：PDF 文字可能含 CJK，若逐位元組跑
// UTF-8，「.」與字元類會在多位元組字元中間切開，命中範圍的位元組長度也會
// 是錯的。回傳的 RegexMatch 仍以 UTF-8 位元組位移／長度表示，與
// domain::SearchResult 的慣例一致，只有引擎內部以碼點運算。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alioth::engine::text {

struct RegexOptions {
    bool caseInsensitive{false};  // 僅 ASCII 摺疊，理由同 utf8_scan.h 的 foldAscii。
};

struct RegexMatch {
    std::size_t byteOffset{0};
    std::size_t byteLength{0};
};

class BoundedRegex {
public:
    BoundedRegex() = default;

    // 編譯失敗（語法錯誤、使用了不支援的構造、或樣式展開後超出安全上限）時
    // valid() 回 false，diagnostic() 說明原因。呼叫端應該把這當成「使用者的
    // 正則式打不開」顯示成一則輸入錯誤，不是內部錯誤。
    [[nodiscard]] static BoundedRegex compile(const std::string& pattern, RegexOptions options = {});

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }

    // 由左至右找出所有不重疊的命中（貪婪最長優先）。text 為空或本身無效時
    // 回傳空陣列。保證時間複雜度 O(text.size() * 本樣式的 NFA 狀態數)。
    [[nodiscard]] std::vector<RegexMatch> findAll(const std::string& text) const;

    // 只找第一個命中，供互動式搜尋（找下一個）使用；語意與 findAll()[0] 相同，
    // 但不必跑完全文。
    [[nodiscard]] bool findFirst(const std::string& text, std::size_t fromByteOffset,
                                 RegexMatch* match) const;

private:
    struct Program;

    bool valid_{false};
    std::string diagnostic_;
    std::shared_ptr<Program> program_;
};

}  // namespace alioth::engine::text
