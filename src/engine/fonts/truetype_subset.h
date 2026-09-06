#pragma once

// TrueType 子集化（ADR-007，PRD-ANN-005／021／022、PRD-FORM-*、PRD-ANN-032）。
//
// 為什麼要自己寫：技術堆疊固定為 Qt 6 + PDFium + OpenSSL 三件（PRD §4.1），
// 不引入 HarfBuzz 或 FreeType。子集化本身是位元組層的表格重建，不需要排版引擎。
//
// 為什麼需要子集化：思源黑體整份約 16 MB，而一份文件用到的漢字通常在數百字內。
// 整份內嵌會讓每個加了一行中文註解的 PDF 都變成 16 MB 起跳。
//
// ## 保留原始 GID，不重新編號
//
// 這是本檔最重要的設計決定。
//
// 子集化的教科書作法是把用到的字重新編號成 0..N-1，讓 loca 表只有 N 項。
// 這裡**刻意不那樣做**：保留原始 glyph index，沒用到的字寫成長度 0 的空字形。
//
// 理由是複合字形（composite glyph）。CJK 字型大量使用複合字形——一個字由
// 部件組合而成，而部件是**用 glyph index 參照**的。重新編號就必須同步改寫每個
// 複合字形內部的參照，漏掉任何一個，那個字會畫出別的字的部件。那種錯不會崩潰，
// 只會讓某幾個字長得很奇怪，而且要在特定文件上才看得到。
//
// 代價是 loca 表維持原長度（思源黑體約 44,000 字，loca 約 176 KB）。
// 每份文件多出約 200 KB，換掉一整類「字長錯了」的缺陷，這筆交易是划算的。
//
// ## 不做的事
//
// 不處理連字、垂直書寫、複雜文字排版——那需要 HarfBuzz，屬於 PRD §2.1 已排除
// 的排版層。這裡只做「一個碼點對應一個字形」的直接對映，CJK 與拉丁都夠用。

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace alioth::engine::fonts {

struct SubsetResult {
    bool ok{false};
    std::string diagnostic;

    // 子集後的字型位元組，可直接放進 PDF 的 /FontFile2。
    std::string bytes;

    // 碼點 → glyph index。呼叫端用它把文字轉成 Identity-H 的雙位元組編碼。
    // 找不到字形的碼點**不會**出現在這裡——呼叫端必須據此明確報錯，
    // 而不是輸出 .notdef，那會在畫面上變成一格空白方塊而使用者不知道為什麼。
    std::map<char32_t, std::uint16_t> glyphForCodepoint;

    // glyph index → 前進寬度（單位是字型設計單位，需除以 unitsPerEm）。
    // PDF 的 /W 陣列由此產生。
    std::map<std::uint16_t, std::uint16_t> advanceForGlyph;

    std::uint16_t unitsPerEm{1000};
    // /FontBBox 用，字型設計單位。
    std::int16_t xMin{0}, yMin{0}, xMax{0}, yMax{0};
    // /Descriptor 需要的度量。
    std::int16_t ascent{0}, descent{0};
};

// 從完整的 TrueType 位元組產生只含 codepoints 所需字形的子集。
//
// fontBytes 必須是 glyf 型的 TrueType（有 glyf/loca 表）。CFF（OpenType/PostScript
// 輪廓）不支援——那是完全不同的輪廓格式，需要另一套子集化程式碼。明確回報而不是
// 產生一份載不進去的字型。
//
// 可變字型（有 fvar 的）會取**預設實例**：直接沿用 glyf 的輪廓，丟掉變化表。
// 那是該字型的預設字重，在 PDF 裡是合法且穩定的結果；保留變化表反而會產生
// 多數檢視器不支援的內嵌字型。
[[nodiscard]] SubsetResult subsetTrueType(const std::string& fontBytes,
                                          const std::set<char32_t>& codepoints);

}  // namespace alioth::engine::fonts
