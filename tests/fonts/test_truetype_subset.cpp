// TrueType 子集化（ADR-007）。
//
// 測試分兩層：
//
//   1. **合成字型**：自己組一份最小但合法的 TrueType，涵蓋簡單字形、複合字形、
//      cmap format 4 與 format 12。這一層在任何機器上都跑得出同樣的結果。
//   2. **真實字型**（若這台機器上有 Noto Sans TC）：驗證子集能吃下一份幾萬字的
//      CJK 字型並顯著縮小。找不到就跳過——它是加分驗證，不是通過條件。
//
// 子集化的錯誤幾乎都是安靜的：字形位置算錯只是畫出別的字，複合字形漏了部件
// 只是少幾筆。因此這裡驗的是位元組結構，不是「有沒有回傳成功」。

#include <QtTest>

#include <QFile>

#include "engine/fonts/truetype_subset.h"

using namespace alioth::engine::fonts;

namespace {

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

[[nodiscard]] std::uint16_t readU16(const std::string& data, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<unsigned char>(data[offset]) << 8) |
                                      static_cast<unsigned char>(data[offset + 1]));
}

[[nodiscard]] std::uint32_t readU32(const std::string& data, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

// 一個簡單字形：一個輪廓、三個點。內容不重要，重要的是長度與能被辨識為簡單字形。
[[nodiscard]] std::string simpleGlyph() {
    std::string glyph;
    appendU16(glyph, 1);   // numberOfContours = 1（正數 → 簡單字形）
    appendU16(glyph, 0);   // xMin
    appendU16(glyph, 0);   // yMin
    appendU16(glyph, 100); // xMax
    appendU16(glyph, 100); // yMax
    appendU16(glyph, 2);   // endPtsOfContours[0] = 2（三個點）
    appendU16(glyph, 0);   // instructionLength
    glyph.push_back(0x01); // flags ×3（隨意，只要長度對）
    glyph.push_back(0x01);
    glyph.push_back(0x01);
    glyph.push_back(0x0A);  // x 座標（1 位元組，SHORT_VECTOR）
    glyph.push_back(0x0A);
    glyph.push_back(0x0A);
    glyph.push_back(0x0A);  // y 座標
    glyph.push_back(0x0A);
    glyph.push_back(0x0A);
    while (glyph.size() % 4 != 0) glyph.push_back('\0');
    return glyph;
}

// 一個複合字形，參照 component 號字形。這是 CJK 字型的常態，
// 也是「保留原始 GID」這個設計決定要保護的東西。
[[nodiscard]] std::string compositeGlyph(std::uint16_t component) {
    std::string glyph;
    appendU16(glyph, static_cast<std::uint16_t>(0xFFFF));  // numberOfContours = -1
    appendU16(glyph, 0);
    appendU16(glyph, 0);
    appendU16(glyph, 100);
    appendU16(glyph, 100);
    appendU16(glyph, 0x0002);  // flags：ARGS_ARE_XY_VALUES，沒有 MORE_COMPONENTS
    appendU16(glyph, component);
    glyph.push_back(0);  // arg1（1 位元組，因為沒設 ARG_1_AND_2_ARE_WORDS）
    glyph.push_back(0);  // arg2
    while (glyph.size() % 4 != 0) glyph.push_back('\0');
    return glyph;
}

struct SyntheticFont {
    std::string bytes;
    std::uint16_t glyphCount{0};
};

// 組一份最小但結構合法的 TrueType。
// 字形配置：0 = .notdef、1 = 'A'、2 = 'B'、3 = 複合字形（參照 1）、4 = 未使用。
[[nodiscard]] SyntheticFont makeFont() {
    const std::uint16_t glyphCount = 5;

    std::vector<std::string> glyphs{simpleGlyph(), simpleGlyph(), simpleGlyph(),
                                    compositeGlyph(1), simpleGlyph()};

    std::string glyf;
    std::vector<std::uint32_t> offsets;
    for (const std::string& glyph : glyphs) {
        offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
        glyf += glyph;
    }
    offsets.push_back(static_cast<std::uint32_t>(glyf.size()));

    std::string loca;  // long 格式
    for (const std::uint32_t offset : offsets) appendU32(loca, offset);

    std::string head;
    appendU32(head, 0x00010000);  // version
    appendU32(head, 0x00010000);  // fontRevision
    appendU32(head, 0);           // checkSumAdjustment
    appendU32(head, 0x5F0F3CF5);  // magicNumber
    appendU16(head, 0);           // flags
    appendU16(head, 1000);        // unitsPerEm
    for (int i = 0; i < 16; ++i) head.push_back('\0');  // created + modified
    appendU16(head, 0);    // xMin
    appendU16(head, 0);    // yMin
    appendU16(head, 1000); // xMax
    appendU16(head, 1000); // yMax
    appendU16(head, 0);    // macStyle
    appendU16(head, 8);    // lowestRecPPEM
    appendU16(head, 2);    // fontDirectionHint
    appendU16(head, 1);    // indexToLocFormat = long
    appendU16(head, 0);    // glyphDataFormat

    std::string hhea;
    appendU32(hhea, 0x00010000);
    appendU16(hhea, 800);   // ascender
    appendU16(hhea, static_cast<std::uint16_t>(-200));  // descender
    appendU16(hhea, 0);     // lineGap
    appendU16(hhea, 1000);  // advanceWidthMax
    for (int i = 0; i < 11; ++i) appendU16(hhea, 0);  // 其餘度量與保留欄位
    appendU16(hhea, glyphCount);  // numberOfHMetrics

    std::string maxp;
    appendU32(maxp, 0x00010000);
    appendU16(maxp, glyphCount);
    for (int i = 0; i < 13; ++i) appendU16(maxp, 0);

    std::string hmtx;
    for (std::uint16_t i = 0; i < glyphCount; ++i) {
        appendU16(hmtx, static_cast<std::uint16_t>(500 + i * 10));  // advanceWidth
        appendU16(hmtx, 0);                                          // leftSideBearing
    }

    // cmap format 4：'A'(0x41)→1、'B'(0x42)→2、0x4E00→3（一個漢字對到複合字形）。
    std::string subtable;
    const std::uint16_t segCount = 4;  // 三段 + 結束段
    appendU16(subtable, 4);            // format
    appendU16(subtable, 0);            // length（稍後回填）
    appendU16(subtable, 0);            // language
    appendU16(subtable, segCount * 2);
    appendU16(subtable, 4);  // searchRange
    appendU16(subtable, 1);  // entrySelector
    appendU16(subtable, 4);  // rangeShift
    // endCode
    appendU16(subtable, 0x42);
    appendU16(subtable, 0x4E00);
    appendU16(subtable, 0xFFFF);
    appendU16(subtable, 0xFFFF);
    appendU16(subtable, 0);  // reservedPad
    // startCode
    appendU16(subtable, 0x41);
    appendU16(subtable, 0x4E00);
    appendU16(subtable, 0xFFFF);
    appendU16(subtable, 0xFFFF);
    // idDelta：glyph = code + delta
    appendU16(subtable, static_cast<std::uint16_t>(1 - 0x41));
    appendU16(subtable, static_cast<std::uint16_t>(3 - 0x4E00));
    appendU16(subtable, 1);
    appendU16(subtable, 1);
    // idRangeOffset
    appendU16(subtable, 0);
    appendU16(subtable, 0);
    appendU16(subtable, 0);
    appendU16(subtable, 0);
    subtable[2] = static_cast<char>((subtable.size() >> 8) & 0xFF);
    subtable[3] = static_cast<char>(subtable.size() & 0xFF);

    std::string cmap;
    appendU16(cmap, 0);  // version
    appendU16(cmap, 1);  // numTables
    appendU16(cmap, 3);  // platformID = Windows
    appendU16(cmap, 1);  // encodingID = Unicode BMP
    appendU32(cmap, 12); // offset
    cmap += subtable;

    struct Entry {
        const char* tag;
        std::string data;
    };
    std::vector<Entry> tables{{"cmap", cmap}, {"glyf", glyf},   {"head", head},
                              {"hhea", hhea}, {"hmtx", hmtx},   {"loca", loca},
                              {"maxp", maxp}};

    std::string out;
    appendU32(out, 0x00010000);
    appendU16(out, static_cast<std::uint16_t>(tables.size()));
    appendU16(out, 64);
    appendU16(out, 2);
    appendU16(out, static_cast<std::uint16_t>(tables.size() * 16 - 64));

    std::uint32_t offset = 12 + static_cast<std::uint32_t>(tables.size()) * 16;
    std::string body;
    std::vector<std::uint32_t> starts;
    for (const Entry& table : tables) {
        starts.push_back(offset + static_cast<std::uint32_t>(body.size()));
        body += table.data;
        while (body.size() % 4 != 0) body.push_back('\0');
    }
    for (std::size_t i = 0; i < tables.size(); ++i) {
        out += std::string(tables[i].tag, 4);
        appendU32(out, 0);  // checksum，解析端不驗
        appendU32(out, starts[i]);
        appendU32(out, static_cast<std::uint32_t>(tables[i].data.size()));
    }
    out += body;

    return SyntheticFont{out, glyphCount};
}

// F-001 回歸測試專用：一份只放 cmap format 4 + format 12 兩張子表的最小字型，
// format 12 裡塞一個會在 32 位元加法上溢位環繞的群組。
[[nodiscard]] std::string buildFontWithOverflowingCmap12() {
    const std::uint16_t glyphCount = 3;  // 0 = .notdef、1 = 'A'、2 = 未使用

    std::vector<std::string> glyphs{simpleGlyph(), simpleGlyph(), simpleGlyph()};
    std::string glyf;
    std::vector<std::uint32_t> offsets;
    for (const std::string& glyph : glyphs) {
        offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
        glyf += glyph;
    }
    offsets.push_back(static_cast<std::uint32_t>(glyf.size()));

    std::string loca;
    for (const std::uint32_t offset : offsets) appendU32(loca, offset);

    std::string head;
    appendU32(head, 0x00010000);
    appendU32(head, 0x00010000);
    appendU32(head, 0);
    appendU32(head, 0x5F0F3CF5);
    appendU16(head, 0);
    appendU16(head, 1000);
    for (int i = 0; i < 16; ++i) head.push_back('\0');
    appendU16(head, 0);
    appendU16(head, 0);
    appendU16(head, 1000);
    appendU16(head, 1000);
    appendU16(head, 0);
    appendU16(head, 8);
    appendU16(head, 2);
    appendU16(head, 1);  // indexToLocFormat = long
    appendU16(head, 0);

    std::string hhea;
    appendU32(hhea, 0x00010000);
    appendU16(hhea, 800);
    appendU16(hhea, static_cast<std::uint16_t>(-200));
    appendU16(hhea, 0);
    appendU16(hhea, 1000);
    for (int i = 0; i < 11; ++i) appendU16(hhea, 0);
    appendU16(hhea, glyphCount);

    std::string maxp;
    appendU32(maxp, 0x00010000);
    appendU16(maxp, glyphCount);
    for (int i = 0; i < 13; ++i) appendU16(maxp, 0);

    std::string hmtx;
    for (std::uint16_t i = 0; i < glyphCount; ++i) {
        appendU16(hmtx, static_cast<std::uint16_t>(500 + i * 10));
        appendU16(hmtx, 0);
    }

    // format 4：只放 'A'(0x41) → 1。讓 unicodeToGlyph 非空，字型本身有效，
    // 這樣測試才是在驗證「format 12 的溢位群組被正確拒絕」，而不是在驗證
    // 「整份字型因為沒有可用 cmap 而報錯」。
    std::string format4;
    appendU16(format4, 4);       // format
    appendU16(format4, 0);       // length（稍後回填）
    appendU16(format4, 0);       // language
    appendU16(format4, 4);       // segCountX2（2 段）
    appendU16(format4, 4);       // searchRange
    appendU16(format4, 1);       // entrySelector
    appendU16(format4, 4);       // rangeShift
    appendU16(format4, 0x41);    // endCode[0]
    appendU16(format4, 0xFFFF);  // endCode[1]
    appendU16(format4, 0);       // reservedPad
    appendU16(format4, 0x41);    // startCode[0]
    appendU16(format4, 0xFFFF);  // startCode[1]
    appendU16(format4, static_cast<std::uint16_t>(1 - 0x41));  // idDelta[0]
    appendU16(format4, 1);                                     // idDelta[1]
    appendU16(format4, 0);       // idRangeOffset[0]
    appendU16(format4, 0);       // idRangeOffset[1]
    format4[2] = static_cast<char>((format4.size() >> 8) & 0xFF);
    format4[3] = static_cast<char>(format4.size() & 0xFF);

    // format 12：一個群組，startGlyphID = 0xFFFFFFF0。
    // 碼點 U+10011（code - start = 0x11）在 32 位元加法下是
    // 0xFFFFFFF0 + 0x11 = 0x100000001，截斷後變成 0x1——看起來像
    // 一個合法的小 glyph index（剛好等於 'A' 真正的 glyph）。
    std::string format12;
    appendU16(format12, 12);              // format
    appendU16(format12, 0);               // reserved
    appendU32(format12, 0);               // length（稍後回填）
    appendU32(format12, 0);               // language
    appendU32(format12, 1);               // numGroups
    appendU32(format12, 0x10000);         // startCharCode
    appendU32(format12, 0x10000 + 0x20);  // endCharCode
    appendU32(format12, 0xFFFFFFF0u);     // startGlyphID
    const auto format12Length = static_cast<std::uint32_t>(format12.size());
    format12[4] = static_cast<char>((format12Length >> 24) & 0xFF);
    format12[5] = static_cast<char>((format12Length >> 16) & 0xFF);
    format12[6] = static_cast<char>((format12Length >> 8) & 0xFF);
    format12[7] = static_cast<char>(format12Length & 0xFF);

    std::string cmap;
    appendU16(cmap, 0);  // version
    appendU16(cmap, 2);  // numTables
    appendU16(cmap, 3);  // platformID
    appendU16(cmap, 1);  // encodingID（Unicode BMP → format 4）
    const std::uint32_t format4Offset = 4 + 2 * 8;
    appendU32(cmap, format4Offset);
    appendU16(cmap, 3);   // platformID
    appendU16(cmap, 10);  // encodingID（Unicode full → format 12）
    const auto format12Offset =
        static_cast<std::uint32_t>(format4Offset + format4.size());
    appendU32(cmap, format12Offset);
    cmap += format4;
    cmap += format12;

    struct Entry {
        const char* tag;
        std::string data;
    };
    std::vector<Entry> tables{{"cmap", cmap}, {"glyf", glyf},   {"head", head},
                              {"hhea", hhea}, {"hmtx", hmtx},   {"loca", loca},
                              {"maxp", maxp}};

    std::string out;
    appendU32(out, 0x00010000);
    appendU16(out, static_cast<std::uint16_t>(tables.size()));
    appendU16(out, 64);
    appendU16(out, 2);
    appendU16(out, static_cast<std::uint16_t>(tables.size() * 16 - 64));

    std::uint32_t offset = 12 + static_cast<std::uint32_t>(tables.size()) * 16;
    std::string body;
    std::vector<std::uint32_t> starts;
    for (const Entry& table : tables) {
        starts.push_back(offset + static_cast<std::uint32_t>(body.size()));
        body += table.data;
        while (body.size() % 4 != 0) body.push_back('\0');
    }
    for (std::size_t i = 0; i < tables.size(); ++i) {
        out += std::string(tables[i].tag, 4);
        appendU32(out, 0);
        appendU32(out, starts[i]);
        appendU32(out, static_cast<std::uint32_t>(tables[i].data.size()));
    }
    out += body;
    return out;
}

}  // namespace

class TestTrueTypeSubset : public QObject {
    Q_OBJECT

private slots:
    void mapsCodepointsToGlyphs() {
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'A', U'B'});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.glyphForCodepoint.at(U'A'), std::uint16_t(1));
        QCOMPARE(result.glyphForCodepoint.at(U'B'), std::uint16_t(2));
        QCOMPARE(result.unitsPerEm, std::uint16_t(1000));
    }

    void unmappedCodepointsAreAbsentRatherThanNotdef() {
        // 對照表裡沒有的碼點，呼叫端才能明確報錯。回傳 0（.notdef）會讓
        // 畫面上出現一格空白方塊，而使用者不知道是缺字還是程式壞了。
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'A', U'Z'});
        QVERIFY(result.ok);
        QVERIFY(result.glyphForCodepoint.count(U'A') == 1);
        QVERIFY(result.glyphForCodepoint.count(U'Z') == 0);
    }

    void keepsOriginalGlyphIndices() {
        // 這是本模組的核心設計決定。重新編號會讓複合字形的部件參照失效。
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'B'});
        QVERIFY(result.ok);
        // 'B' 在原字型是 2 號，子集後仍然要是 2 號。
        QCOMPARE(result.glyphForCodepoint.at(U'B'), std::uint16_t(2));
        // loca 的項數不變（原始編號保留），因此仍是 glyphCount + 1 項。
        const std::string& bytes = result.bytes;
        const std::uint16_t tableCount = readU16(bytes, 4);
        bool foundLoca = false;
        for (std::uint16_t i = 0; i < tableCount; ++i) {
            const std::size_t at = 12 + static_cast<std::size_t>(i) * 16;
            if (bytes.compare(at, 4, "loca") != 0) continue;
            foundLoca = true;
            QCOMPARE(readU32(bytes, at + 12),
                     std::uint32_t((font.glyphCount + 1) * 4));  // long 格式，每項 4 位元組
        }
        QVERIFY(foundLoca);
    }

    void compositeGlyphPullsInItsComponents() {
        // 0x4E00 對到 3 號複合字形，它參照 1 號。子集必須把 1 號一起帶上，
        // 否則那個字會少一半——不崩潰，只是畫出來缺筆畫。
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'\u4E00'});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.glyphForCodepoint.at(U'\u4E00'), std::uint16_t(3));
        // 1 號被帶進來了：它的寬度會出現在 advance 對照表裡。
        QVERIFY(result.advanceForGlyph.count(1) == 1);
        QVERIFY(result.advanceForGlyph.count(3) == 1);
    }

    void unusedGlyphsAreDroppedFromGlyf() {
        const SyntheticFont font = makeFont();
        const SubsetResult all =
            subsetTrueType(font.bytes, {U'A', U'B', U'\u4E00'});
        const SubsetResult one = subsetTrueType(font.bytes, {U'A'});
        QVERIFY(all.ok && one.ok);
        // 只留一個字的子集必須比較小。相等代表根本沒有丟掉任何字形。
        QVERIFY2(one.bytes.size() < all.bytes.size(),
                 qPrintable(QStringLiteral("一個字 %1 位元組、三個字 %2 位元組")
                                .arg(one.bytes.size())
                                .arg(all.bytes.size())));
    }

    void notdefIsAlwaysKept() {
        // PDF 規格要求 0 號字形存在。
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'A'});
        QVERIFY(result.ok);
        QVERIFY(result.advanceForGlyph.count(0) == 1);
    }

    void outputUsesLongLocaFormat() {
        // short 格式的位移上限是 128 KB，CJK 子集很容易超過，而超過之後是
        // 靜默截斷——字形位置全錯。因此一律輸出 long。
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'A'});
        QVERIFY(result.ok);

        const std::string& bytes = result.bytes;
        const std::uint16_t tableCount = readU16(bytes, 4);
        for (std::uint16_t i = 0; i < tableCount; ++i) {
            const std::size_t at = 12 + static_cast<std::size_t>(i) * 16;
            if (bytes.compare(at, 4, "head") != 0) continue;
            const std::uint32_t offset = readU32(bytes, at + 8);
            QCOMPARE(readU16(bytes, offset + 50), std::uint16_t(1));
            return;
        }
        QFAIL("子集字型裡找不到 head 表");
    }

    void tableDirectoryIsSortedByTag() {
        // 規格要求表目錄依 tag 排序。不排序在多數解析器上仍然能用，
        // 但驗證工具會抱怨，而且我們自己的 qpdf 檢查也可能翻臉。
        const SyntheticFont font = makeFont();
        const SubsetResult result = subsetTrueType(font.bytes, {U'A'});
        QVERIFY(result.ok);
        const std::uint16_t tableCount = readU16(result.bytes, 4);
        QVERIFY(tableCount > 1);
        for (std::uint16_t i = 1; i < tableCount; ++i) {
            const std::string previous = result.bytes.substr(12 + (i - 1) * 16, 4);
            const std::string current = result.bytes.substr(12 + i * 16, 4);
            QVERIFY2(previous < current,
                     qPrintable(QStringLiteral("表目錄未排序：%1 出現在 %2 之前")
                                    .arg(QString::fromStdString(previous))
                                    .arg(QString::fromStdString(current))));
        }
    }

    void cmapFormat12OverflowIsRejectedNotAliased() {
        // F-001：startGlyph + (code - start) 若在 32 位元上溢位，環繞後可能
        // 變成一個看似合法、實際上與另一個真實字形撞號的小 glyph index。
        // 必須明確找不到，不可以悄悄疊到別的字上。
        const std::string font = buildFontWithOverflowingCmap12();
        const SubsetResult result =
            subsetTrueType(font, {U'A', static_cast<char32_t>(0x10011)});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.glyphForCodepoint.at(U'A'), std::uint16_t(1));
        QVERIFY(result.glyphForCodepoint.count(static_cast<char32_t>(0x10011)) == 0);
    }

    void cffFontsAreRejectedExplicitly() {
        // OTTO 是 CFF 輪廓，與 glyf 完全不同。回報而不是產生一份載不進去的字型。
        std::string otto = "OTTO";
        otto.resize(64, '\0');
        const SubsetResult result = subsetTrueType(otto, {U'A'});
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("CFF") != std::string::npos);
    }

    void collectionsAreRejectedExplicitly() {
        std::string collection = "ttcf";
        collection.resize(64, '\0');
        const SubsetResult result = subsetTrueType(collection, {U'A'});
        QVERIFY(!result.ok);
    }

    void garbageIsRejectedWithoutCrashing() {
        QVERIFY(!subsetTrueType("", {U'A'}).ok);
        QVERIFY(!subsetTrueType(std::string(200, '\x01'), {U'A'}).ok);
    }

    // 真實字型的加分驗證。找不到就跳過——它不是通過條件，
    // 但它是唯一能證明「幾萬字的 CJK 字型真的縮得下來」的檢查。
    void realCjkFontSubsetsToASmallFraction() {
        const QString path = QStringLiteral("C:/Windows/Fonts/NotoSansTC-VF.ttf");
        if (!QFile::exists(path)) {
            QSKIP("這台機器上沒有 Noto Sans TC，跳過真實字型驗證");
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray raw = file.readAll();
        file.close();
        const std::string fontBytes(raw.constData(), static_cast<std::size_t>(raw.size()));

        const std::set<char32_t> text{U'測', U'試', U'中', U'文', U'註', U'解', U'A', U'1'};
        const SubsetResult result = subsetTrueType(fontBytes, text);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        for (const char32_t codepoint : text) {
            QVERIFY2(result.glyphForCodepoint.count(codepoint) == 1,
                     qPrintable(QStringLiteral("碼點 U+%1 沒有對到字形")
                                    .arg(static_cast<std::uint32_t>(codepoint), 4, 16)));
        }

        qInfo("真實字型：原始 %lld 位元組 → 子集 %lld 位元組",
              static_cast<long long>(fontBytes.size()),
              static_cast<long long>(result.bytes.size()));
        // 八個字的子集不該超過原始字型的四分之一。整份思源黑體約 16 MB，
        // 而 loca 表（保留原始編號的代價）約 176 KB，其餘是那八個字的輪廓。
        QVERIFY(result.bytes.size() < fontBytes.size() / 4);
    }
};

QTEST_APPLESS_MAIN(TestTrueTypeSubset)
#include "test_truetype_subset.moc"
