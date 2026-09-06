#include "engine/fonts/cid_font_writer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace alioth::engine::fonts {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfStream;

// 字型設計單位 → PDF 的千分之一 em。PDF 的字型度量一律以 1000 為單位，
// 與字型自己的 unitsPerEm 無關（常見是 1000 或 2048）。
[[nodiscard]] std::int64_t toThousandths(std::uint16_t value, std::uint16_t unitsPerEm) {
    if (unitsPerEm == 0) return value;
    return static_cast<std::int64_t>(
        (static_cast<double>(value) * 1000.0 / static_cast<double>(unitsPerEm)) + 0.5);
}

[[nodiscard]] std::int64_t toThousandthsSigned(std::int16_t value, std::uint16_t unitsPerEm) {
    if (unitsPerEm == 0) return value;
    const double scaled = static_cast<double>(value) * 1000.0 / static_cast<double>(unitsPerEm);
    return static_cast<std::int64_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

void appendHex4(std::string& out, std::uint32_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04X", value & 0xFFFFu);
    out += buffer;
}

// /ToUnicode CMap。沒有它，Identity-H 編碼的文字複製出來是亂碼、搜尋也找不到——
// 使用者會覺得「中文看得到但複製不出來」，那看起來像檔案壞了。
[[nodiscard]] std::string buildToUnicodeCMap(const SubsetResult& subset) {
    // 反向對照：一個 glyph 可能對應多個碼點（異體字），取最小的那個即可——
    // /ToUnicode 是給「複製出來要看得懂」用的，不是可逆對映。
    std::map<std::uint16_t, char32_t> unicodeForGlyph;
    for (const auto& [codepoint, glyph] : subset.glyphForCodepoint) {
        auto it = unicodeForGlyph.find(glyph);
        if (it == unicodeForGlyph.end() || codepoint < it->second) {
            unicodeForGlyph[glyph] = codepoint;
        }
    }

    std::string cmap;
    cmap +=
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n";

    // bfchar 一段最多 100 項，這是 CMap 的規格限制。超過要分段，
    // 否則某些解析器會在第 101 項靜默停止——後面的字複製出來就是亂碼。
    std::vector<std::pair<std::uint16_t, char32_t>> entries(unicodeForGlyph.begin(),
                                                            unicodeForGlyph.end());
    for (std::size_t start = 0; start < entries.size(); start += 100) {
        const std::size_t count = std::min<std::size_t>(100, entries.size() - start);
        cmap += std::to_string(count) + " beginbfchar\n";
        for (std::size_t i = 0; i < count; ++i) {
            const auto& [glyph, codepoint] = entries[start + i];
            cmap += "<";
            appendHex4(cmap, glyph);
            cmap += "> <";
            if (codepoint > 0xFFFF) {
                // 輔助平面要寫成 UTF-16 代理對。直接寫五位十六進位是無效的，
                // 而多數工具會安靜地讀成別的字。
                const char32_t value = codepoint - 0x10000;
                appendHex4(cmap, 0xD800 + (value >> 10));
                appendHex4(cmap, 0xDC00 + (value & 0x3FF));
            } else {
                appendHex4(cmap, static_cast<std::uint32_t>(codepoint));
            }
            cmap += ">\n";
        }
        cmap += "endbfchar\n";
    }

    cmap +=
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n";
    return cmap;
}

// /W 陣列。格式是 [ 起始CID [ 寬度... ] 起始CID 結束CID 寬度 ... ]，
// 這裡只用第一種形式，並把連續的 CID 併成一組——一個字一組會讓
// 幾百個字的陣列長得沒有必要。
[[nodiscard]] PdfArray buildWidths(const SubsetResult& subset) {
    PdfArray widths;
    std::vector<std::uint16_t> glyphs;
    glyphs.reserve(subset.advanceForGlyph.size());
    for (const auto& [glyph, advance] : subset.advanceForGlyph) glyphs.push_back(glyph);
    std::sort(glyphs.begin(), glyphs.end());

    std::size_t i = 0;
    while (i < glyphs.size()) {
        std::size_t run = i + 1;
        while (run < glyphs.size() && glyphs[run] == glyphs[run - 1] + 1) ++run;

        widths.emplace_back(static_cast<std::int64_t>(glyphs[i]));
        PdfArray group;
        for (std::size_t k = i; k < run; ++k) {
            group.emplace_back(
                toThousandths(subset.advanceForGlyph.at(glyphs[k]), subset.unitsPerEm));
        }
        widths.push_back(PdfObject{std::move(group)});
        i = run;
    }
    return widths;
}

}  // namespace

std::string subsetTag(const std::string& fontBytes) {
    // FNV-1a。不需要密碼學強度——這只是要讓「同樣的子集得到同樣的前綴」，
    // 而那是為了讓同一份文件重複存檔時不會每次都產生新的字型名稱。
    std::uint64_t hash = 1469598103934665603ull;
    for (const char byte : fontBytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    std::string tag;
    for (int i = 0; i < 6; ++i) {
        tag.push_back(static_cast<char>('A' + (hash % 26)));
        hash /= 26;
    }
    return tag;
}

bool encodeIdentityH(const SubsetResult& subset, const std::u32string& text, std::string& out) {
    out.clear();
    out.reserve(text.size() * 4);
    for (const char32_t codepoint : text) {
        const auto it = subset.glyphForCodepoint.find(codepoint);
        if (it == subset.glyphForCodepoint.end()) return false;
        const std::uint16_t glyph = it->second;
        // 兩位元組 big-endian。跳脫 PDF 字串字面值裡有特殊意義的位元組——
        // 括號與反斜線在任何位置都要跳脫，否則字串會在那裡提前結束，
        // 而其後的所有物件都會解析錯位。
        for (const int shift : {8, 0}) {
            const auto byte = static_cast<unsigned char>((glyph >> shift) & 0xFF);
            if (byte == '(' || byte == ')' || byte == '\\') {
                out.push_back('\\');
                out.push_back(static_cast<char>(byte));
            } else if (byte < 32 || byte > 126) {
                // 非可列印位元組寫成三位八進位。寫成原始位元組在多數情況下也
                // 合法，但 \r 會被某些解析器正規化成 \n，那會讓 GID 直接變成別的字。
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\%03o", byte);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(byte));
            }
        }
    }
    return true;
}

EmbeddedFontResult embedSubsetFont(objects::IncrementalAppender& appender,
                                   const SubsetResult& subset, const std::string& baseName) {
    return embedSubsetFont(
        ObjectSink{[&appender](objects::PdfObject object) {
            const int number = appender.allocateObject();
            appender.setObject(number, std::move(object));
            return number;
        }},
        subset, baseName);
}

EmbeddedFontResult embedSubsetFont(const ObjectSink& sink, const SubsetResult& subset,
                                   const std::string& baseName) {
    EmbeddedFontResult result;
    if (!subset.ok) {
        result.diagnostic = "字型子集化未成功，不寫入";
        return result;
    }
    if (subset.glyphForCodepoint.empty()) {
        result.diagnostic = "子集裡沒有任何字形對照";
        return result;
    }

    const std::string fullName = subsetTag(subset.bytes) + "+" + baseName;

    // FontFile2：子集後的 TrueType 位元組。
    PdfStream fontFile;
    fontFile.dict.set("Length1", PdfObject{static_cast<std::int64_t>(subset.bytes.size())});
    fontFile.data = subset.bytes;
    const int fontFileNumber = sink(PdfObject{std::move(fontFile)});

    // FontDescriptor。
    PdfDictionary descriptor;
    descriptor.set("Type", objects::makeName("FontDescriptor"));
    descriptor.set("FontName", objects::makeName(fullName));
    // 4 = Symbolic。CJK 字型不落在 PDF 的標準拉丁編碼裡，宣告成 Nonsymbolic
    // 會讓部分檢視器試著套用標準編碼並顯示錯字。
    descriptor.set("Flags", PdfObject{static_cast<std::int64_t>(4)});
    PdfArray bbox;
    bbox.emplace_back(toThousandthsSigned(subset.xMin, subset.unitsPerEm));
    bbox.emplace_back(toThousandthsSigned(subset.yMin, subset.unitsPerEm));
    bbox.emplace_back(toThousandthsSigned(subset.xMax, subset.unitsPerEm));
    bbox.emplace_back(toThousandthsSigned(subset.yMax, subset.unitsPerEm));
    descriptor.set("FontBBox", PdfObject{std::move(bbox)});
    descriptor.set("ItalicAngle", PdfObject{static_cast<std::int64_t>(0)});
    descriptor.set("Ascent", PdfObject{toThousandthsSigned(subset.ascent, subset.unitsPerEm)});
    descriptor.set("Descent", PdfObject{toThousandthsSigned(subset.descent, subset.unitsPerEm)});
    // CapHeight 與 StemV 是必填但字型檔裡不一定有。給保守的估計值而不是省略——
    // 省略會讓部分檢視器拒絕載入整個字型，而估錯只影響字型替換時的相似度。
    descriptor.set("CapHeight", PdfObject{static_cast<std::int64_t>(700)});
    descriptor.set("StemV", PdfObject{static_cast<std::int64_t>(80)});
    descriptor.set("FontFile2", PdfObject{objects::PdfRef{fontFileNumber, 0}});
    const int descriptorNumber = sink(PdfObject{std::move(descriptor)});

    // CIDFontType2（子字型）。
    PdfDictionary cidFont;
    cidFont.set("Type", objects::makeName("Font"));
    cidFont.set("Subtype", objects::makeName("CIDFontType2"));
    cidFont.set("BaseFont", objects::makeName(fullName));
    PdfDictionary systemInfo;
    systemInfo.set("Registry", PdfObject{objects::PdfString{"Adobe"}});
    systemInfo.set("Ordering", PdfObject{objects::PdfString{"Identity"}});
    systemInfo.set("Supplement", PdfObject{static_cast<std::int64_t>(0)});
    cidFont.set("CIDSystemInfo", PdfObject{std::move(systemInfo)});
    cidFont.set("FontDescriptor", PdfObject{objects::PdfRef{descriptorNumber, 0}});
    // /CIDToGIDMap /Identity：CID 就是 GID。這是「保留原始 glyph index」那個
    // 子集化決定的另一半——兩者必須一致，否則每個字都會畫成別的字。
    cidFont.set("CIDToGIDMap", objects::makeName("Identity"));
    cidFont.set("DW", PdfObject{static_cast<std::int64_t>(1000)});
    cidFont.set("W", PdfObject{buildWidths(subset)});
    const int cidFontNumber = sink(PdfObject{std::move(cidFont)});

    // ToUnicode CMap。
    PdfStream toUnicode;
    toUnicode.data = buildToUnicodeCMap(subset);
    const int toUnicodeNumber = sink(PdfObject{std::move(toUnicode)});

    // Type0 字型（外層）。
    PdfDictionary type0;
    type0.set("Type", objects::makeName("Font"));
    type0.set("Subtype", objects::makeName("Type0"));
    type0.set("BaseFont", objects::makeName(fullName));
    type0.set("Encoding", objects::makeName("Identity-H"));
    PdfArray descendants;
    descendants.emplace_back(objects::PdfRef{cidFontNumber, 0});
    type0.set("DescendantFonts", PdfObject{std::move(descendants)});
    type0.set("ToUnicode", PdfObject{objects::PdfRef{toUnicodeNumber, 0}});
    const int type0Number = sink(PdfObject{std::move(type0)});

    result.ok = true;
    result.fontObject = type0Number;
    return result;
}

}  // namespace alioth::engine::fonts
