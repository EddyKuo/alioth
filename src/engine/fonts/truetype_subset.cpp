#include "engine/fonts/truetype_subset.h"

#include <algorithm>
#include <cstring>

namespace alioth::engine::fonts {
namespace {

// TrueType 一律是大端序，與本機位元組序無關。逐位元組組出來而不是 memcpy 後
// 交換——後者在對齊不足的位址上是未定義行為，而字型表在檔案裡不保證對齊。
[[nodiscard]] std::uint16_t readU16(const std::string& data, std::size_t offset) {
    if (offset + 2 > data.size()) return 0;
    return static_cast<std::uint16_t>((static_cast<unsigned char>(data[offset]) << 8) |
                                      static_cast<unsigned char>(data[offset + 1]));
}

[[nodiscard]] std::int16_t readS16(const std::string& data, std::size_t offset) {
    return static_cast<std::int16_t>(readU16(data, offset));
}

[[nodiscard]] std::uint32_t readU32(const std::string& data, std::size_t offset) {
    if (offset + 4 > data.size()) return 0;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

void appendU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

struct TableRecord {
    std::uint32_t checksum{0};
    std::uint32_t offset{0};
    std::uint32_t length{0};
};

[[nodiscard]] std::uint32_t tagOf(const char* text) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]));
}

// cmap 子表 format 4（BMP）。CJK 的基本區與拉丁都在這裡。
void readCmapFormat4(const std::string& data, std::size_t base,
                     std::map<char32_t, std::uint16_t>& out) {
    const std::uint16_t segCountX2 = readU16(data, base + 6);
    const std::uint16_t segCount = segCountX2 / 2;
    if (segCount == 0) return;

    const std::size_t endCodes = base + 14;
    const std::size_t startCodes = endCodes + segCountX2 + 2;  // +2 是保留欄位
    const std::size_t idDeltas = startCodes + segCountX2;
    const std::size_t idRangeOffsets = idDeltas + segCountX2;

    for (std::uint16_t segment = 0; segment < segCount; ++segment) {
        const std::uint16_t end = readU16(data, endCodes + segment * 2);
        const std::uint16_t start = readU16(data, startCodes + segment * 2);
        if (start > end) continue;
        const std::int16_t delta = readS16(data, idDeltas + segment * 2);
        const std::size_t rangeOffsetAt = idRangeOffsets + segment * 2;
        const std::uint16_t rangeOffset = readU16(data, rangeOffsetAt);

        for (std::uint32_t code = start; code <= end; ++code) {
            // 0xFFFF 是結束標記，不是真的字元。
            if (code == 0xFFFF) continue;
            std::uint16_t glyph = 0;
            if (rangeOffset == 0) {
                glyph = static_cast<std::uint16_t>((code + delta) & 0xFFFF);
            } else {
                // 規格定義的間接尋址：位移是從 idRangeOffset 欄位本身算起的。
                const std::size_t at = rangeOffsetAt + rangeOffset + (code - start) * 2;
                glyph = readU16(data, at);
                if (glyph != 0) glyph = static_cast<std::uint16_t>((glyph + delta) & 0xFFFF);
            }
            if (glyph != 0) out.emplace(static_cast<char32_t>(code), glyph);
        }
    }
}

// cmap 子表 format 12（含輔助平面）。表情符號與罕用漢字在這裡。
void readCmapFormat12(const std::string& data, std::size_t base,
                      std::map<char32_t, std::uint16_t>& out) {
    const std::uint32_t groups = readU32(data, base + 12);
    for (std::uint32_t i = 0; i < groups; ++i) {
        const std::size_t at = base + 16 + i * 12;
        const std::uint32_t start = readU32(data, at);
        const std::uint32_t end = readU32(data, at + 4);
        const std::uint32_t startGlyph = readU32(data, at + 8);
        if (end < start || end - start > 0x10FFFF) continue;
        for (std::uint32_t code = start; code <= end; ++code) {
            const std::uint32_t glyph = startGlyph + (code - start);
            if (glyph != 0 && glyph <= 0xFFFF) {
                out.emplace(static_cast<char32_t>(code), static_cast<std::uint16_t>(glyph));
            }
        }
    }
}

// 複合字形會用 glyph index 參照部件。子集必須把部件一起帶上，
// 否則那個字會少一半——不會崩潰，只是畫出來缺筆畫。
void collectComponents(const std::string& glyf, std::uint32_t glyphOffset,
                       std::uint32_t glyphLength, std::set<std::uint16_t>& needed,
                       int depth) {
    // 複合字形可以巢狀，惡意或損壞的字型可以做出循環參照。限深而不是靠
    // 「已訪問集合」——後者在正常字型上也會多配置一份集合，而深度 8 遠超過
    // 任何真實字型的巢狀層數。
    if (depth > 8 || glyphLength < 10) return;

    const std::int16_t contours = readS16(glyf, glyphOffset);
    if (contours >= 0) return;  // 簡單字形，沒有部件

    std::size_t at = glyphOffset + 10;
    for (;;) {
        if (at + 4 > glyphOffset + glyphLength) return;
        const std::uint16_t flags = readU16(glyf, at);
        const std::uint16_t component = readU16(glyf, at + 2);
        needed.insert(component);
        at += 4;

        // ARG_1_AND_2_ARE_WORDS
        at += (flags & 0x0001) ? 4 : 2;
        if (flags & 0x0008) {          // WE_HAVE_A_SCALE
            at += 2;
        } else if (flags & 0x0040) {   // WE_HAVE_AN_X_AND_Y_SCALE
            at += 4;
        } else if (flags & 0x0080) {   // WE_HAVE_A_TWO_BY_TWO
            at += 8;
        }
        if ((flags & 0x0020) == 0) break;  // MORE_COMPONENTS
    }
}

[[nodiscard]] std::uint32_t tableChecksum(const std::string& table) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < table.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            word <<= 8;
            if (i + k < table.size()) word |= static_cast<unsigned char>(table[i + k]);
        }
        sum += word;
    }
    return sum;
}

}  // namespace

SubsetResult subsetTrueType(const std::string& fontBytes, const std::set<char32_t>& codepoints) {
    SubsetResult result;

    if (fontBytes.size() < 12) {
        result.diagnostic = "字型檔太小，不是有效的 TrueType";
        return result;
    }

    const std::uint32_t version = readU32(fontBytes, 0);
    // 0x00010000 是 TrueType，'true' 是舊 Mac 格式。'OTTO' 是 CFF 輪廓，
    // 那是完全不同的輪廓格式，需要另一套子集化程式碼——明確拒絕而不是
    // 產生一份載不進去的字型。
    if (version == tagOf("OTTO")) {
        result.diagnostic = "這是 CFF（OpenType/PostScript）輪廓的字型，目前只支援 glyf 輪廓";
        return result;
    }
    if (version == tagOf("ttcf")) {
        result.diagnostic = "這是 TrueType Collection，請先取出其中單一字型";
        return result;
    }
    if (version != 0x00010000u && version != tagOf("true")) {
        result.diagnostic = "無法辨識的字型格式";
        return result;
    }

    const std::uint16_t tableCount = readU16(fontBytes, 4);
    std::map<std::uint32_t, TableRecord> tables;
    for (std::uint16_t i = 0; i < tableCount; ++i) {
        const std::size_t at = 12 + static_cast<std::size_t>(i) * 16;
        if (at + 16 > fontBytes.size()) break;
        TableRecord record;
        const std::uint32_t tag = readU32(fontBytes, at);
        record.checksum = readU32(fontBytes, at + 4);
        record.offset = readU32(fontBytes, at + 8);
        record.length = readU32(fontBytes, at + 12);
        if (record.offset + record.length > fontBytes.size()) continue;
        tables.emplace(tag, record);
    }

    const auto tableBytes = [&](const char* tag) -> std::string {
        const auto it = tables.find(tagOf(tag));
        if (it == tables.end()) return {};
        return fontBytes.substr(it->second.offset, it->second.length);
    };

    const std::string head = tableBytes("head");
    const std::string maxp = tableBytes("maxp");
    const std::string hhea = tableBytes("hhea");
    const std::string hmtx = tableBytes("hmtx");
    const std::string loca = tableBytes("loca");
    const std::string glyf = tableBytes("glyf");
    const std::string cmap = tableBytes("cmap");

    if (head.size() < 54 || maxp.size() < 6 || hhea.size() < 36 || loca.empty() || glyf.empty()) {
        result.diagnostic = "字型缺少必要的表（head/maxp/hhea/loca/glyf）";
        return result;
    }

    result.unitsPerEm = readU16(head, 18);
    if (result.unitsPerEm == 0) result.unitsPerEm = 1000;
    result.xMin = readS16(head, 36);
    result.yMin = readS16(head, 38);
    result.xMax = readS16(head, 40);
    result.yMax = readS16(head, 42);
    const std::uint16_t indexToLocFormat = readU16(head, 50);
    result.ascent = readS16(hhea, 4);
    result.descent = readS16(hhea, 6);
    const std::uint16_t numberOfHMetrics = readU16(hhea, 34);
    const std::uint16_t numGlyphs = readU16(maxp, 4);

    if (numGlyphs == 0) {
        result.diagnostic = "字型沒有任何字形";
        return result;
    }

    // 讀 loca：short 格式存的是「位移除以 2」，long 格式存實際位移。
    std::vector<std::uint32_t> offsets(static_cast<std::size_t>(numGlyphs) + 1, 0);
    for (std::uint32_t i = 0; i <= numGlyphs; ++i) {
        offsets[i] = indexToLocFormat == 0
                         ? static_cast<std::uint32_t>(readU16(loca, i * 2)) * 2
                         : readU32(loca, i * 4);
    }

    // cmap：挑一個 Unicode 子表。優先 format 12（含輔助平面），退而求其次 format 4。
    std::map<char32_t, std::uint16_t> unicodeToGlyph;
    if (!cmap.empty()) {
        const std::uint16_t subtables = readU16(cmap, 2);
        std::size_t best4 = 0;
        std::size_t best12 = 0;
        for (std::uint16_t i = 0; i < subtables; ++i) {
            const std::size_t at = 4 + static_cast<std::size_t>(i) * 8;
            const std::uint16_t platform = readU16(cmap, at);
            const std::uint16_t encoding = readU16(cmap, at + 2);
            const std::uint32_t offset = readU32(cmap, at + 4);
            if (offset >= cmap.size()) continue;
            const std::uint16_t format = readU16(cmap, offset);
            const bool unicodePlatform =
                platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
            if (!unicodePlatform) continue;
            if (format == 12 && best12 == 0) best12 = offset;
            if (format == 4 && best4 == 0) best4 = offset;
        }
        if (best12 != 0) readCmapFormat12(cmap, best12, unicodeToGlyph);
        if (best4 != 0) {
            // format 4 只補 format 12 沒有的碼點：emplace 不覆蓋既有項目。
            readCmapFormat4(cmap, best4, unicodeToGlyph);
        }
    }
    if (unicodeToGlyph.empty()) {
        result.diagnostic = "字型沒有可用的 Unicode cmap 子表";
        return result;
    }

    // 要保留的字形。0 號（.notdef）一定留著——PDF 規格要求它存在。
    std::set<std::uint16_t> needed{0};
    for (const char32_t codepoint : codepoints) {
        const auto it = unicodeToGlyph.find(codepoint);
        if (it == unicodeToGlyph.end()) continue;  // 找不到就不放進對照表，呼叫端據此報錯
        needed.insert(it->second);
        result.glyphForCodepoint.emplace(codepoint, it->second);
    }

    // 展開複合字形的部件。要迭代到收斂——部件本身也可能是複合字形。
    for (int pass = 0; pass < 8; ++pass) {
        std::set<std::uint16_t> discovered = needed;
        for (const std::uint16_t glyph : needed) {
            if (glyph + 1u >= offsets.size()) continue;
            const std::uint32_t start = offsets[glyph];
            const std::uint32_t end = offsets[glyph + 1];
            if (end <= start || end > glyf.size()) continue;
            collectComponents(glyf, start, end - start, discovered, pass);
        }
        if (discovered.size() == needed.size()) break;
        needed = std::move(discovered);
    }

    // 新的 glyf 與 loca。保留原始編號，沒用到的寫成長度 0。
    std::string newGlyf;
    std::vector<std::uint32_t> newOffsets(static_cast<std::size_t>(numGlyphs) + 1, 0);
    for (std::uint32_t glyph = 0; glyph < numGlyphs; ++glyph) {
        newOffsets[glyph] = static_cast<std::uint32_t>(newGlyf.size());
        if (needed.count(static_cast<std::uint16_t>(glyph)) == 0) continue;
        const std::uint32_t start = offsets[glyph];
        const std::uint32_t end = offsets[glyph + 1];
        if (end <= start || end > glyf.size()) continue;
        newGlyf.append(glyf, start, end - start);
        // 字形資料必須 4 位元組對齊（long loca 格式的慣例，短格式更是必須，
        // 因為它存的是位移除以 2）。
        while (newGlyf.size() % 4 != 0) newGlyf.push_back('\0');
    }
    newOffsets[numGlyphs] = static_cast<std::uint32_t>(newGlyf.size());

    // 一律輸出 long loca：short 格式的位移上限是 128 KB，CJK 子集很容易超過，
    // 而超過之後是靜默截斷——字形位置全錯，畫出來是一堆亂碼。
    std::string newLoca;
    newLoca.reserve((static_cast<std::size_t>(numGlyphs) + 1) * 4);
    for (std::uint32_t i = 0; i <= numGlyphs; ++i) appendU32(newLoca, newOffsets[i]);

    // head 要把 indexToLocFormat 改成 1（long）。
    std::string newHead = head;
    newHead[50] = 0;
    newHead[51] = 1;
    // checkSumAdjustment 歸零：它是整份檔案的校驗和，我們改了內容之後舊值無效。
    // 多數檢視器不檢查它，但留著一個明確錯誤的值不如寫 0。
    for (int i = 8; i < 12; ++i) newHead[static_cast<std::size_t>(i)] = 0;

    // hmtx：寬度照抄，並記下需要的字形寬度供 PDF 的 /W 陣列使用。
    for (const std::uint16_t glyph : needed) {
        std::uint16_t advance = 0;
        if (glyph < numberOfHMetrics) {
            advance = readU16(hmtx, static_cast<std::size_t>(glyph) * 4);
        } else if (numberOfHMetrics > 0) {
            // 超過 numberOfHMetrics 的字形共用最後一個寬度，這是 hmtx 的壓縮規則。
            advance = readU16(hmtx, static_cast<std::size_t>(numberOfHMetrics - 1) * 4);
        }
        result.advanceForGlyph.emplace(glyph, advance);
    }

    // 組出子集字型。只放 PDF 內嵌真正需要的表。
    //
    // 刻意不放 cmap：Identity-H 編碼直接以 CID 當 GID，PDF 不透過字型的 cmap
    // 查字。放了只是白白多幾十 KB，而且一份與 /ToUnicode 可能不一致的 cmap
    // 會讓某些工具的文字擷取結果與我們寫的 /ToUnicode 打架。
    struct Output {
        const char* tag;
        std::string data;
    };
    std::vector<Output> outputs{
        {"head", newHead},
        {"hhea", hhea},
        {"maxp", maxp},
        {"hmtx", hmtx},
        {"loca", newLoca},
        {"glyf", newGlyf},
    };
    // 提示指令相關的表若存在就一起帶走：少了它們，低解析度下的字會糊掉。
    for (const char* tag : {"cvt ", "fpgm", "prep", "gasp"}) {
        std::string data = tableBytes(tag);
        if (!data.empty()) outputs.push_back({tag, std::move(data)});
    }
    // 表目錄必須依 tag 排序（規格要求）。
    std::sort(outputs.begin(), outputs.end(), [](const Output& a, const Output& b) {
        return std::strncmp(a.tag, b.tag, 4) < 0;
    });

    const auto count = static_cast<std::uint16_t>(outputs.size());
    std::uint16_t searchRange = 16;
    std::uint16_t entrySelector = 0;
    while (static_cast<std::uint32_t>(searchRange) * 2 <= static_cast<std::uint32_t>(count) * 16) {
        searchRange = static_cast<std::uint16_t>(searchRange * 2);
        ++entrySelector;
    }

    std::string out;
    appendU32(out, 0x00010000u);
    appendU16(out, count);
    appendU16(out, searchRange);
    appendU16(out, entrySelector);
    appendU16(out, static_cast<std::uint16_t>(count * 16 - searchRange));

    std::uint32_t offset = 12 + static_cast<std::uint32_t>(count) * 16;
    std::string body;
    std::vector<std::uint32_t> tableOffsets;
    for (const Output& table : outputs) {
        tableOffsets.push_back(offset + static_cast<std::uint32_t>(body.size()));
        body += table.data;
        while (body.size() % 4 != 0) body.push_back('\0');
    }

    for (std::size_t i = 0; i < outputs.size(); ++i) {
        out.push_back(outputs[i].tag[0]);
        out.push_back(outputs[i].tag[1]);
        out.push_back(outputs[i].tag[2]);
        out.push_back(outputs[i].tag[3]);
        appendU32(out, tableChecksum(outputs[i].data));
        appendU32(out, tableOffsets[i]);
        appendU32(out, static_cast<std::uint32_t>(outputs[i].data.size()));
    }
    out += body;

    result.ok = true;
    result.bytes = std::move(out);
    return result;
}

}  // namespace alioth::engine::fonts
