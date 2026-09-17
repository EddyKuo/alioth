#pragma once

// 從其他來源建立 PDF 的領域模型（PRD-IO-009 / IO-010 / IO-012，WBS 15）。
//
// 為什麼這一層存在：三種來源真正難的部分都不是 PDF 語法，而是「這份輸入
// 攤在紙上應該長什麼樣」——一張 300 dpi 的照片該印成多大、一段沒有換行的
// 長文在 A4 上會斷在哪個字、Markdown 的第三層清單要縮排幾點、標題落在第幾頁
// 的哪個高度。那些全部可以在不產生任何位元組的情況下定義與驗證，因此放在
// 領域層；引擎層只剩「把算好的版面翻成內容串流與物件」這一件事。
//
// header-only 是刻意的：本檔沒有跨翻譯單元的狀態，不進 src/domain/CMakeLists.txt
// 就不會讓其他工作包多背一次重編譯。
//
// 只處理 ASCII 是刻意的，不是偷懶。標準 14 字型走 WinAnsiEncoding，畫不出
// CJK；而 CJK 字型內嵌的授權策略在 CLAUDE.md 仍是待決策項（影響 WBS 4.8）。
// 在那之前，唯一誠實的作法是明確失敗——靜默輸出會得到一整頁看不出原因的
// 空白或方框，使用者只會以為是我們的程式壞了。
//
// 座標一律是 PDF 使用者空間：原點左下、Y 軸向上、單位為點（1/72 吋）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain/csv.h"
#include "geometry.h"

namespace alioth::domain::create {

// ---------------------------------------------------------------------------
// 紙張
// ---------------------------------------------------------------------------

struct PaperSize {
    double widthPt{595.276};
    double heightPt{841.89};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return widthPt > 1.0 && heightPt > 1.0;
    }
};

inline constexpr PaperSize kPaperA4{595.276, 841.89};
inline constexpr PaperSize kPaperLetter{612.0, 792.0};

// ---------------------------------------------------------------------------
// 影像來源（PRD-IO-009）
// ---------------------------------------------------------------------------

// JpegEncoded 是「已經是 JPEG 位元組」而不是某種像素排列。它與其他三者並列，
// 因為整條匯入路徑對它的處理方式從頭到尾都不同：不解碼、不重編碼、直接以
// DCTDecode 塞進 PDF。重新編碼一次 JPEG 會同時損失畫質與時間，而掃描器
// 匯入的典型輸入正好全是 JPEG。
enum class ImagePixelFormat {
    Gray8,
    Rgb8,
    Rgba8,
    JpegEncoded,
};

struct SourceImage {
    int width{0};
    int height{0};
    ImagePixelFormat format{ImagePixelFormat::Rgb8};
    std::vector<std::uint8_t> bytes;

    // 掃描器與相機都會寫 dpi；沒有的話由呼叫端填一個明確的預設值，
    // 而不是讓這一層猜——猜錯的症狀是整份文件的尺寸都不對。
    double dpiX{96.0};
    double dpiY{96.0};

    // JpegEncoded 時才有意義：灰階 JPEG 的 /ColorSpace 必須是 DeviceGray，
    // 寫成 DeviceRGB 會讓每三個取樣被當成一個像素，畫面變成彩色雜訊。
    bool jpegGray{false};

    [[nodiscard]] bool hasAlpha() const noexcept {
        return format == ImagePixelFormat::Rgba8;
    }

    [[nodiscard]] std::size_t componentsPerPixel() const noexcept {
        switch (format) {
            case ImagePixelFormat::Gray8:
                return 1;
            case ImagePixelFormat::Rgb8:
                return 3;
            case ImagePixelFormat::Rgba8:
                return 4;
            case ImagePixelFormat::JpegEncoded:
                return 0;
        }
        return 0;
    }

    [[nodiscard]] bool valid() const noexcept {
        if (width <= 0 || height <= 0 || bytes.empty()) return false;
        if (format == ImagePixelFormat::JpegEncoded) return true;
        const std::size_t expected = static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * componentsPerPixel();
        return bytes.size() == expected;
    }
};

enum class ImagePageSizing {
    // 頁面跟著影像走：一張 300 dpi、3000×2400 的掃描頁會得到 10×8 吋的頁面，
    // 列印出來與原稿等大。這是掃描匯入的正確預設。
    FromImageDpi,
    // 頁面固定，影像等比縮放後置中。相簿式的輸出需要它。
    FixedPaper,
};

struct ImageImportOptions {
    ImagePageSizing sizing{ImagePageSizing::FromImageDpi};
    PaperSize paper{kPaperA4};
    double marginPt{0.0};

    // 影像本身沒帶 dpi 時用的值。0 或負數視為無效並改用 72。
    double fallbackDpi{96.0};

    // 小圖是否放大到填滿版面。預設不放大：把一張 100×100 的圖撐成整頁 A4
    // 只會得到一片馬賽克，而使用者通常沒預期我們會這樣做。
    bool allowUpscale{false};
};

struct ImagePlacement {
    double pageWidthPt{0.0};
    double pageHeightPt{0.0};
    RectF rect{};  // 影像在頁面上的目標矩形（PDF 座標）
};

[[nodiscard]] inline ImagePlacement computeImagePlacement(const SourceImage& image,
                                                          const ImageImportOptions& options) {
    ImagePlacement placement;
    if (image.width <= 0 || image.height <= 0) return placement;

    const double margin = std::max(0.0, options.marginPt);
    const double dpiX = image.dpiX > 0.0 ? image.dpiX
                                         : (options.fallbackDpi > 0.0 ? options.fallbackDpi : 72.0);
    const double dpiY = image.dpiY > 0.0 ? image.dpiY
                                         : (options.fallbackDpi > 0.0 ? options.fallbackDpi : 72.0);

    const double naturalW = static_cast<double>(image.width) / dpiX * 72.0;
    const double naturalH = static_cast<double>(image.height) / dpiY * 72.0;

    if (options.sizing == ImagePageSizing::FromImageDpi) {
        placement.pageWidthPt = naturalW + margin * 2.0;
        placement.pageHeightPt = naturalH + margin * 2.0;
        placement.rect = RectF{margin, margin, margin + naturalW, margin + naturalH};
        return placement;
    }

    const PaperSize paper = options.paper.valid() ? options.paper : kPaperA4;
    placement.pageWidthPt = paper.widthPt;
    placement.pageHeightPt = paper.heightPt;

    const double availW = std::max(1.0, paper.widthPt - margin * 2.0);
    const double availH = std::max(1.0, paper.heightPt - margin * 2.0);
    double scale = std::min(availW / naturalW, availH / naturalH);
    if (!options.allowUpscale) scale = std::min(scale, 1.0);

    const double drawW = naturalW * scale;
    const double drawH = naturalH * scale;
    const double left = (paper.widthPt - drawW) / 2.0;
    const double bottom = (paper.heightPt - drawH) / 2.0;
    placement.rect = RectF{left, bottom, left + drawW, bottom + drawH};
    return placement;
}

// JPEG 標頭探測。/Width /Height /ColorSpace 必須與 DCTDecode 資料裡的實際值
// 一致，否則多數檢視器顯示成扭曲的斜條紋而不是報錯。呼叫端如果已知尺寸可以
// 不用它，但「以為知道」正是這類錯誤的來源，所以提供一個能對帳的來源。
struct JpegProbe {
    bool ok{false};
    int width{0};
    int height{0};
    int components{0};  // 1 = 灰階、3 = YCbCr、4 = CMYK/YCCK
};

[[nodiscard]] inline JpegProbe probeJpeg(const std::uint8_t* data, std::size_t size) {
    JpegProbe probe;
    if (data == nullptr || size < 4) return probe;
    if (data[0] != 0xFF || data[1] != 0xD8) return probe;

    std::size_t i = 2;
    while (i + 3 < size) {
        if (data[i] != 0xFF) {
            ++i;  // 填充位元組；規格允許連續的 0xFF
            continue;
        }
        const std::uint8_t marker = data[i + 1];
        if (marker == 0xFF) {
            ++i;
            continue;
        }
        // 無酬載的標記（RSTn、SOI、EOI、TEM）。
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (i + 3 >= size) break;
        const std::size_t segLen =
            (static_cast<std::size_t>(data[i + 2]) << 8) | static_cast<std::size_t>(data[i + 3]);
        // SOFn。0xC4（DHT）、0xC8、0xCC 不是框架標頭，必須排除。
        const bool isSof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                           marker != 0xC8 && marker != 0xCC;
        if (isSof) {
            if (i + 9 >= size) break;
            probe.height = (static_cast<int>(data[i + 5]) << 8) | static_cast<int>(data[i + 6]);
            probe.width = (static_cast<int>(data[i + 7]) << 8) | static_cast<int>(data[i + 8]);
            probe.components = static_cast<int>(data[i + 9]);
            probe.ok = probe.width > 0 && probe.height > 0 && probe.components > 0;
            return probe;
        }
        if (segLen < 2) break;
        i += 2 + segLen;
    }
    return probe;
}

[[nodiscard]] inline JpegProbe probeJpeg(const std::vector<std::uint8_t>& bytes) {
    return probeJpeg(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// 標準 14 字型度量
// ---------------------------------------------------------------------------

// 為什麼這裡有一份完整的字寬表，而 formbuild/field_appearance.h 只有粗估：
// 兩者的用途不同。表單欄位只用它決定單行文字的對齊偏移，差幾點看不出來；
// 這裡要用它決定「這一行斷在哪個字」，估寬偏大會提早換行、偏小會超出邊界
// 印到紙外。斷行是版面的骨架，不能建立在估算上。
enum class StandardFont {
    Helvetica,
    HelveticaBold,
    HelveticaOblique,
    HelveticaBoldOblique,
    Courier,
    CourierBold,
    CourierOblique,
};

[[nodiscard]] inline const char* baseFontName(StandardFont font) noexcept {
    switch (font) {
        case StandardFont::Helvetica:
            return "Helvetica";
        case StandardFont::HelveticaBold:
            return "Helvetica-Bold";
        case StandardFont::HelveticaOblique:
            return "Helvetica-Oblique";
        case StandardFont::HelveticaBoldOblique:
            return "Helvetica-BoldOblique";
        case StandardFont::Courier:
            return "Courier";
        case StandardFont::CourierBold:
            return "Courier-Bold";
        case StandardFont::CourierOblique:
            return "Courier-Oblique";
    }
    return "Helvetica";
}

[[nodiscard]] inline bool isFixedPitch(StandardFont font) noexcept {
    return font == StandardFont::Courier || font == StandardFont::CourierBold ||
           font == StandardFont::CourierOblique;
}

// AFM 字寬，單位 1/1000 em，索引為 ASCII 0x20..0x7E。
inline constexpr int kHelveticaWidths[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

inline constexpr int kHelveticaBoldWidths[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
    975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
    333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

// 回傳單一字元的字寬（1/1000 em）。非 ASCII 一律回傳 0：斷行不該替一個
// 我們畫不出來的字元保留空間，而那個字元本來就會在 scanAscii 被擋下。
[[nodiscard]] inline double glyphWidth1000(StandardFont font, unsigned char ch) noexcept {
    if (isFixedPitch(font)) return (ch >= 0x20 && ch <= 0x7E) ? 600.0 : 0.0;
    if (ch < 0x20 || ch > 0x7E) return 0.0;
    const int index = static_cast<int>(ch) - 0x20;
    const bool bold =
        font == StandardFont::HelveticaBold || font == StandardFont::HelveticaBoldOblique;
    return static_cast<double>(bold ? kHelveticaBoldWidths[index] : kHelveticaWidths[index]);
}

[[nodiscard]] inline double measureText(StandardFont font, std::string_view text,
                                        double fontSize) noexcept {
    double units = 0.0;
    for (const char c : text) units += glyphWidth1000(font, static_cast<unsigned char>(c));
    return units / 1000.0 * fontSize;
}

// ---------------------------------------------------------------------------
// ASCII 檢查
// ---------------------------------------------------------------------------

struct AsciiScan {
    bool ok{true};
    std::size_t offset{0};    // 第一個不可接受位元組的位置
    unsigned char byte{0};

    // 給使用者看的說明。SDD §7 要求降級與失敗都必須明確可見，
    // 只回一個 false 等於要人自己去猜是哪一個字有問題。
    [[nodiscard]] std::string describe() const {
        if (ok) return {};
        std::string message = "第 " + std::to_string(offset) + " 個位元組是 0x";
        const char* digits = "0123456789ABCDEF";
        message.push_back(digits[(byte >> 4) & 0x0F]);
        message.push_back(digits[byte & 0x0F]);
        message +=
            "，超出標準 14 字型可表示的範圍。本轉換路徑尚未接上 ADR-007 的內嵌"
            "子集（屬 R3 的 PRD-IO-013），因此明確失敗而不輸出缺字的頁面。";
        return message;
    }
};

// 可接受的是可列印 ASCII 加上 \n / \r / \t / \f。其餘控制字元一律拒絕：
// 它們畫不出來，但也不像 CJK 那樣有「將來會支援」的路徑，靜默丟掉會讓輸出
// 與輸入對不上。
//
// \f（換頁符）是排版指令而不是可見字元，layoutPlainText 把它當成分頁點。
// ASCII 對「換頁」本來就只有這一個表示法，自創一個標記（例如一行 "---"）
// 會與文件內容本身撞在一起。
[[nodiscard]] inline AsciiScan scanAscii(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n' || c == '\r' || c == '\t' || c == '\f') continue;
        if (c >= 0x20 && c <= 0x7E) continue;
        return AsciiScan{false, i, c};
    }
    return AsciiScan{};
}

// ---------------------------------------------------------------------------
// 純文字版面（PRD-IO-012）
// ---------------------------------------------------------------------------

struct TextImportOptions {
    PaperSize paper{kPaperA4};
    double marginPt{54.0};  // 0.75 吋
    double fontSize{11.0};
    double leadingRatio{1.25};
    bool monospace{false};  // 程式碼與日誌用 Courier，等寬才對得齊欄位
    int tabWidth{4};
};

struct TextLayout {
    bool ok{false};
    std::string diagnostic;

    std::vector<std::vector<std::string>> pages;  // 每頁的每一行（已斷行）
    StandardFont font{StandardFont::Helvetica};
    double fontSize{11.0};
    double leading{13.75};
    double leftX{54.0};
    double firstBaselineY{0.0};
    double pageWidthPt{0.0};
    double pageHeightPt{0.0};

    [[nodiscard]] std::size_t pageCount() const noexcept { return pages.size(); }
    [[nodiscard]] std::size_t lineCount() const noexcept {
        std::size_t total = 0;
        for (const auto& page : pages) total += page.size();
        return total;
    }
};

namespace detail {

// 貪婪斷行。行寬不足以容納單一「字」時才在字元中間硬切——那通常是一長串
// URL 或雜湊值，切開比讓它衝出頁面邊界好。
inline void wrapParagraph(std::string_view paragraph, StandardFont font, double fontSize,
                          double maxWidth, std::vector<std::string>& out) {
    if (paragraph.empty()) {
        out.emplace_back();
        return;
    }

    // 整行放得下就原樣輸出，不經過重排。純文字的縮排與對齊本身就是資訊
    // （日誌、程式碼、表格式的欄位），而下面的貪婪演算法會把連續空白
    // 壓成一個——那對需要換行的長行是必要的取捨，對放得下的行則是純損失。
    if (measureText(font, paragraph, fontSize) <= maxWidth) {
        out.emplace_back(paragraph);
        return;
    }

    std::string line;
    std::size_t i = 0;
    while (i < paragraph.size()) {
        // 取下一個「空白 + 字」的單位，讓行首不會留下空白。
        std::size_t wordStart = i;
        while (wordStart < paragraph.size() && paragraph[wordStart] == ' ') ++wordStart;
        std::size_t wordEnd = wordStart;
        while (wordEnd < paragraph.size() && paragraph[wordEnd] != ' ') ++wordEnd;
        if (wordStart == wordEnd) {
            // 尾端只剩空白。
            break;
        }
        const std::string_view word = paragraph.substr(wordStart, wordEnd - wordStart);

        std::string candidate = line;
        if (!candidate.empty()) candidate.push_back(' ');
        candidate.append(word);

        if (measureText(font, candidate, fontSize) <= maxWidth) {
            line = std::move(candidate);
            i = wordEnd;
            continue;
        }

        if (!line.empty()) {
            out.push_back(line);
            line.clear();
            i = wordStart;  // 同一個字改放到下一行重試
            continue;
        }

        // 空行仍放不下這個字：逐字元硬切。
        std::string piece;
        std::size_t consumed = 0;
        for (const char c : word) {
            std::string next = piece;
            next.push_back(c);
            if (!piece.empty() && measureText(font, next, fontSize) > maxWidth) break;
            piece = std::move(next);
            ++consumed;
        }
        if (consumed == 0) consumed = 1, piece.assign(1, word[0]);
        out.push_back(piece);
        i = wordStart + consumed;
    }

    if (!line.empty() || out.empty()) out.push_back(line);
}

inline std::string expandTabs(std::string_view text, int tabWidth) {
    if (tabWidth <= 0) tabWidth = 4;
    std::string out;
    out.reserve(text.size());
    int column = 0;
    for (const char c : text) {
        if (c == '\t') {
            const int spaces = tabWidth - (column % tabWidth);
            out.append(static_cast<std::size_t>(spaces), ' ');
            column += spaces;
        } else {
            out.push_back(c);
            ++column;
        }
    }
    return out;
}

inline std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

// 依 \f 切段。結尾的 \f 不會產生一個空段——「最後一段結束」與「後面還有
// 一個空頁」在使用者眼中完全不同，而多出來的空白頁是看得見的錯。
[[nodiscard]] inline std::vector<std::string_view> splitFormFeed(std::string_view text) {
    std::vector<std::string_view> blocks;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\f') continue;
        blocks.push_back(text.substr(start, i - start));
        start = i + 1;
    }
    if (start < text.size() || blocks.empty()) blocks.push_back(text.substr(start));
    return blocks;
}

// 版心高度能放幾行。分頁與斷行必須看同一個數字，分別各算一次就會在
// 某些行距下差一行，而症狀是最後一頁莫名多出或少掉一行。
[[nodiscard]] inline std::size_t linesPerPage(const TextLayout& layout) noexcept {
    const double contentHeight = layout.pageHeightPt - layout.leftX * 2.0;
    return static_cast<std::size_t>(std::max(1.0, std::floor(contentHeight / layout.leading)));
}

// 分頁骨架：\f 分段、段內逐行斷行、修剪尾端空行、依版心高度切頁。
//
// 斷行方式由呼叫端給（ASCII 走標準 14 字型的字寬表，含 CJK 的輸入走內嵌
// 字型的字寬），但「一段文字怎麼變成幾頁」只有這一份實作。兩份的代價是
// 往後任何一次調整邊距、行距或尾端空行規則，都必須記得同時改另一邊，
// 而漏掉的症狀是 ASCII 與 CJK 的同一份文件分頁不一樣。
//
// wrapLine 回傳 false 代表這一行排不下（例如版心容不下單一字形），
// 整個版面隨即放棄，診斷由呼叫端自己填。
template <typename WrapLine>
[[nodiscard]] inline bool paginate(std::string_view text, std::size_t perPage,
                                   std::vector<std::vector<std::string>>& pages,
                                   WrapLine wrapLine) {
    for (const std::string_view block : splitFormFeed(text)) {
        std::vector<std::string> allLines;
        for (const std::string& raw : splitLines(block)) {
            if (!wrapLine(raw, allLines)) return false;
        }
        // 尾端的空行不值得多印一頁。
        while (allLines.size() > 1 && allLines.back().empty()) allLines.pop_back();

        for (std::size_t i = 0; i < allLines.size(); i += perPage) {
            const std::size_t end = std::min(allLines.size(), i + perPage);
            pages.emplace_back(allLines.begin() + static_cast<std::ptrdiff_t>(i),
                               allLines.begin() + static_cast<std::ptrdiff_t>(end));
        }
    }
    if (pages.empty()) pages.emplace_back();
    return true;
}

}  // namespace detail

[[nodiscard]] inline TextLayout layoutPlainText(std::string_view text,
                                                const TextImportOptions& options = {}) {
    TextLayout layout;
    const AsciiScan scan = scanAscii(text);
    if (!scan.ok) {
        layout.diagnostic = scan.describe();
        return layout;
    }

    const PaperSize paper = options.paper.valid() ? options.paper : kPaperA4;
    const double fontSize = options.fontSize > 0.0 ? options.fontSize : 11.0;
    const double margin = std::max(0.0, options.marginPt);
    const double contentWidth = paper.widthPt - margin * 2.0;
    const double contentHeight = paper.heightPt - margin * 2.0;
    if (contentWidth < fontSize || contentHeight < fontSize) {
        layout.diagnostic = "邊距過大，扣掉之後沒有可以放文字的區域";
        return layout;
    }

    layout.font = options.monospace ? StandardFont::Courier : StandardFont::Helvetica;
    layout.fontSize = fontSize;
    layout.leading = fontSize * (options.leadingRatio > 0.0 ? options.leadingRatio : 1.25);
    layout.leftX = margin;
    layout.pageWidthPt = paper.widthPt;
    layout.pageHeightPt = paper.heightPt;
    // 基線不是頂端：字的上緣要在版心之內，否則第一行會被切掉半個字高。
    // 0.75 em 是標準 14 字型 ascender 的保守值。
    layout.firstBaselineY = paper.heightPt - margin - fontSize * 0.75;

    // \f 分段：每一段各自從新的一頁開始，段內再依版心高度自動分頁。
    // 沒有 \f 的輸入只有一段，行為與先前完全相同。
    // ASCII 的斷行不會失敗（放不下的字改成逐字元硬切），回傳值恆為 true。
    (void)detail::paginate(text, detail::linesPerPage(layout), layout.pages,
                     [&](const std::string& raw, std::vector<std::string>& out) {
                         detail::wrapParagraph(detail::expandTabs(raw, options.tabWidth),
                                               layout.font, fontSize, contentWidth, out);
                         return true;
                     });

    layout.ok = true;
    return layout;
}

// ---------------------------------------------------------------------------
// Markdown（PRD-IO-012）
// ---------------------------------------------------------------------------

// 支援範圍刻意只到「不需要版面引擎」的那條線為止：標題、段落、清單、
// 程式碼區塊、行內強調、水平線。表格與圖片內嵌不做——表格要欄寬協商與
// 跨頁表頭重複，圖片要與文字流交互避讓，兩者都需要一套真正的版面引擎，
// 成本高於本工作包其餘所有部分的總和，而 PRD 只把 Markdown 匯入列為 S/R2。
enum class MarkdownBlockKind {
    Heading,
    Paragraph,
    ListItem,
    CodeBlock,
    HorizontalRule,
};

struct InlineSpan {
    std::string text;
    bool bold{false};
    bool italic{false};
    bool code{false};
};

struct MarkdownBlock {
    MarkdownBlockKind kind{MarkdownBlockKind::Paragraph};
    int headingLevel{0};   // Heading 時為 1..3
    int listDepth{0};      // ListItem 時為 0 起算的巢狀層
    bool ordered{false};
    std::string marker;    // 清單項目的項目符號文字（"-" 或 "1."）
    std::vector<InlineSpan> spans;
    std::vector<std::string> codeLines;
    std::string codeLanguage;
};

namespace detail {

[[nodiscard]] inline std::string_view trimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return trimRight(s);
}

inline void pushSpan(std::vector<InlineSpan>& spans, std::string text, bool bold, bool italic,
                     bool code) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().bold == bold && spans.back().italic == italic &&
        spans.back().code == code) {
        spans.back().text += text;
        return;
    }
    spans.push_back(InlineSpan{std::move(text), bold, italic, code});
}

// 行內標記解析。刻意不做巢狀強調的完整 CommonMark 語意：**a *b* c** 這種
// 組合在真實文件裡罕見，而完整實作需要 delimiter run 的配對演算法。
// 這裡採「遇到成對標記就切換狀態」的線性掃描，對常見寫法結果一致。
[[nodiscard]] inline std::vector<InlineSpan> parseInline(std::string_view text) {
    std::vector<InlineSpan> spans;
    std::string buffer;
    bool bold = false;
    bool italic = false;
    bool code = false;

    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];

        if (c == '\\' && i + 1 < text.size()) {
            // 跳脫：\* 要印出星號本身，不能當成強調標記。
            buffer.push_back(text[i + 1]);
            i += 2;
            continue;
        }

        if (c == '`') {
            pushSpan(spans, buffer, bold, italic, code);
            buffer.clear();
            code = !code;
            ++i;
            continue;
        }

        if (!code && (c == '*' || c == '_')) {
            const bool doubled = (i + 1 < text.size() && text[i + 1] == c);
            pushSpan(spans, buffer, bold, italic, code);
            buffer.clear();
            if (doubled) {
                bold = !bold;
                i += 2;
            } else {
                italic = !italic;
                i += 1;
            }
            continue;
        }

        buffer.push_back(c);
        ++i;
    }
    pushSpan(spans, buffer, bold, italic, code);
    return spans;
}

[[nodiscard]] inline bool isHorizontalRule(std::string_view line) {
    const std::string_view t = trim(line);
    if (t.size() < 3) return false;
    const char c = t.front();
    if (c != '-' && c != '*' && c != '_') return false;
    for (const char ch : t) {
        if (ch != c && ch != ' ') return false;
    }
    return true;
}

}  // namespace detail

[[nodiscard]] inline std::vector<MarkdownBlock> parseMarkdown(std::string_view source) {
    std::vector<MarkdownBlock> blocks;
    const std::vector<std::string> lines = detail::splitLines(source);

    std::vector<std::string> paragraph;
    auto flushParagraph = [&]() {
        if (paragraph.empty()) return;
        std::string joined;
        for (std::size_t i = 0; i < paragraph.size(); ++i) {
            if (i > 0) joined.push_back(' ');
            joined += paragraph[i];
        }
        MarkdownBlock block;
        block.kind = MarkdownBlockKind::Paragraph;
        block.spans = detail::parseInline(joined);
        blocks.push_back(std::move(block));
        paragraph.clear();
    };

    bool inCode = false;
    MarkdownBlock codeBlock;

    for (const std::string& raw : lines) {
        const std::string_view line = detail::trimRight(raw);
        const std::string_view trimmed = detail::trim(line);

        if (trimmed.size() >= 3 && trimmed.substr(0, 3) == "```") {
            if (inCode) {
                blocks.push_back(std::move(codeBlock));
                codeBlock = MarkdownBlock{};
                inCode = false;
            } else {
                flushParagraph();
                inCode = true;
                codeBlock = MarkdownBlock{};
                codeBlock.kind = MarkdownBlockKind::CodeBlock;
                codeBlock.codeLanguage = std::string(detail::trim(trimmed.substr(3)));
            }
            continue;
        }
        if (inCode) {
            codeBlock.codeLines.emplace_back(line);
            continue;
        }

        if (trimmed.empty()) {
            flushParagraph();
            continue;
        }

        if (detail::isHorizontalRule(trimmed)) {
            flushParagraph();
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::HorizontalRule;
            blocks.push_back(std::move(block));
            continue;
        }

        if (trimmed.front() == '#') {
            std::size_t level = 0;
            while (level < trimmed.size() && trimmed[level] == '#') ++level;
            if (level >= 1 && level <= 6 && level < trimmed.size() && trimmed[level] == ' ') {
                flushParagraph();
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Heading;
                // h4 以下降級成 h3：再細分的字級差異在 11pt 內文旁邊看不出來，
                // 而書籤層級太深反而難用。
                block.headingLevel = static_cast<int>(std::min<std::size_t>(level, 3));
                block.spans = detail::parseInline(detail::trim(trimmed.substr(level + 1)));
                blocks.push_back(std::move(block));
                continue;
            }
        }

        // 清單。縮排每 2 個空白算一層，這是最常見的手寫習慣。
        std::size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        const std::string_view body = line.substr(indent);
        bool isUnordered = body.size() >= 2 && (body[0] == '-' || body[0] == '*' || body[0] == '+') &&
                           body[1] == ' ';
        std::size_t digits = 0;
        while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9') ++digits;
        const bool isOrdered = digits > 0 && digits + 1 < body.size() && body[digits] == '.' &&
                               body[digits + 1] == ' ';
        if (isUnordered || isOrdered) {
            flushParagraph();
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::ListItem;
            block.listDepth = static_cast<int>(indent / 2);
            block.ordered = isOrdered;
            if (isOrdered) {
                block.marker = std::string(body.substr(0, digits + 1));
                block.spans = detail::parseInline(detail::trim(body.substr(digits + 2)));
            } else {
                // 項目符號一律正規化成連字號：原始碼裡的 -、*、+ 三種寫法
                // 在語意上完全相同，保留差異只會讓輸出隨著作者的打字習慣變動。
                block.marker = "-";
                block.spans = detail::parseInline(detail::trim(body.substr(2)));
            }
            blocks.push_back(std::move(block));
            continue;
        }

        paragraph.emplace_back(trimmed);
    }

    if (inCode) blocks.push_back(std::move(codeBlock));
    flushParagraph();
    return blocks;
}

// --- Markdown 版面 ---

struct MarkdownImportOptions {
    PaperSize paper{kPaperA4};
    double marginPt{54.0};
    double bodyFontSize{11.0};
    double leadingRatio{1.35};
    double headingScale1{1.9};
    double headingScale2{1.5};
    double headingScale3{1.2};
    double blockSpacingPt{7.0};
    double codeFontSize{9.5};
    double listIndentPt{16.0};
    double ruleThicknessPt{0.75};
};

// 已定位的一段文字。版面層算好一切，引擎層只負責翻成 Tf/Td/Tj。
struct PlacedRun {
    std::string text;
    StandardFont font{StandardFont::Helvetica};
    double fontSize{11.0};
    double xPt{0.0};
    double baselineYPt{0.0};
};

struct PlacedRule {
    double xPt{0.0};
    double yPt{0.0};
    double widthPt{0.0};
    double thicknessPt{0.75};
};

struct MarkdownPage {
    std::vector<PlacedRun> runs;
    std::vector<PlacedRule> rules;
};

// 書籤「意圖」而不是書籤本身。
//
// 這裡刻意只輸出資料，不呼叫 engine/bookmarks 的寫入器：那條相依會把整個
// 大綱子系統（outline_writer、destination_codec、incremental_appender）拖進
// 這個 target，而本 target 的其他三種來源一個都用不到它。呼叫端若要書籤，
// 拿這份意圖去餵 outline_writer 即可；不要的話就什麼都不必連結。
struct BookmarkIntent {
    std::string title;
    int level{1};       // 1..3，對應 h1..h3
    int pageIndex{0};   // 0 起算
    double topYPt{0.0}; // 目的地的 /XYZ top
};

struct MarkdownLayout {
    bool ok{false};
    std::string diagnostic;
    std::vector<MarkdownPage> pages;
    std::vector<BookmarkIntent> bookmarks;
    double pageWidthPt{0.0};
    double pageHeightPt{0.0};

    [[nodiscard]] std::size_t pageCount() const noexcept { return pages.size(); }
};

namespace detail {

[[nodiscard]] inline StandardFont spanFont(const InlineSpan& span, bool heading) {
    if (span.code) return StandardFont::Courier;
    if (heading) return StandardFont::HelveticaBold;
    if (span.bold && span.italic) return StandardFont::HelveticaBoldOblique;
    if (span.bold) return StandardFont::HelveticaBold;
    if (span.italic) return StandardFont::HelveticaOblique;
    return StandardFont::Helvetica;
}

// 把一串行內片段折成多行。片段會在空白處拆開，因此同一個 span 可以跨行，
// 粗體不會因為換行而中斷——那是純字串斷行做不到的事。
struct FlowFragment {
    std::string text;
    StandardFont font;
    double fontSize;
    double width;
};

[[nodiscard]] inline std::vector<FlowFragment> toFragments(const std::vector<InlineSpan>& spans,
                                                           bool heading, double fontSize) {
    std::vector<FlowFragment> fragments;
    for (const InlineSpan& span : spans) {
        const StandardFont font = spanFont(span, heading);
        std::string word;
        auto flush = [&]() {
            if (word.empty()) return;
            fragments.push_back(
                FlowFragment{word, font, fontSize, measureText(font, word, fontSize)});
            word.clear();
        };
        for (const char c : span.text) {
            if (c == ' ') {
                flush();
                fragments.push_back(
                    FlowFragment{" ", font, fontSize, measureText(font, " ", fontSize)});
            } else {
                word.push_back(c);
            }
        }
        flush();
    }
    return fragments;
}

}  // namespace detail

[[nodiscard]] inline MarkdownLayout layoutMarkdown(std::string_view source,
                                                   const MarkdownImportOptions& options = {}) {
    MarkdownLayout layout;
    const AsciiScan scan = scanAscii(source);
    if (!scan.ok) {
        layout.diagnostic = scan.describe();
        return layout;
    }

    const PaperSize paper = options.paper.valid() ? options.paper : kPaperA4;
    const double margin = std::max(0.0, options.marginPt);
    const double contentWidth = paper.widthPt - margin * 2.0;
    const double bodySize = options.bodyFontSize > 0.0 ? options.bodyFontSize : 11.0;
    if (contentWidth < bodySize * 4.0 || paper.heightPt - margin * 2.0 < bodySize * 2.0) {
        layout.diagnostic = "邊距過大，扣掉之後沒有可以排版的區域";
        return layout;
    }

    layout.pageWidthPt = paper.widthPt;
    layout.pageHeightPt = paper.heightPt;
    layout.pages.emplace_back();

    const double bottomLimit = margin;
    double cursorY = paper.heightPt - margin;

    auto newPage = [&]() {
        layout.pages.emplace_back();
        cursorY = paper.heightPt - margin;
    };
    auto ensureRoom = [&](double needed) {
        if (cursorY - needed < bottomLimit && !layout.pages.back().runs.empty()) newPage();
    };

    for (const MarkdownBlock& block : parseMarkdown(source)) {
        switch (block.kind) {
            case MarkdownBlockKind::HorizontalRule: {
                const double needed = options.blockSpacingPt * 2.0;
                ensureRoom(needed);
                cursorY -= options.blockSpacingPt;
                layout.pages.back().rules.push_back(
                    PlacedRule{margin, cursorY, contentWidth, options.ruleThicknessPt});
                cursorY -= options.blockSpacingPt;
                break;
            }

            case MarkdownBlockKind::CodeBlock: {
                const double size = options.codeFontSize > 0.0 ? options.codeFontSize : 9.5;
                const double leading = size * 1.25;
                cursorY -= options.blockSpacingPt;
                for (const std::string& line : block.codeLines) {
                    ensureRoom(leading);
                    cursorY -= leading;
                    if (!line.empty()) {
                        layout.pages.back().runs.push_back(PlacedRun{
                            line, StandardFont::Courier, size, margin + options.listIndentPt / 2.0,
                            cursorY});
                    }
                }
                cursorY -= options.blockSpacingPt;
                break;
            }

            case MarkdownBlockKind::Heading:
            case MarkdownBlockKind::Paragraph:
            case MarkdownBlockKind::ListItem: {
                const bool heading = block.kind == MarkdownBlockKind::Heading;
                double size = bodySize;
                if (heading) {
                    const double scale = block.headingLevel == 1   ? options.headingScale1
                                         : block.headingLevel == 2 ? options.headingScale2
                                                                   : options.headingScale3;
                    size = bodySize * scale;
                }
                const double leading = size * options.leadingRatio;

                double indent = 0.0;
                std::string bullet;
                if (block.kind == MarkdownBlockKind::ListItem) {
                    indent = options.listIndentPt * static_cast<double>(block.listDepth + 1);
                    // 項目符號用 ASCII 的連字號而不是 U+2022：WinAnsiEncoding 的
                    // bullet 在 0x95，不同檢視器對它的字型替換結果不一致，
                    // 而連字號在任何情況下都畫得出來。
                    bullet = block.ordered ? block.marker : "-";
                }

                const double textLeft = margin + indent;
                const double availWidth = contentWidth - indent;

                std::vector<detail::FlowFragment> fragments =
                    detail::toFragments(block.spans, heading, size);

                cursorY -= options.blockSpacingPt;

                std::size_t index = 0;
                bool firstLine = true;
                while (index < fragments.size() || firstLine) {
                    // 行首的空白片段丟掉，否則換行處會多出一格縮排。
                    while (index < fragments.size() && fragments[index].text == " ") ++index;
                    if (index >= fragments.size() && !firstLine) break;

                    std::vector<detail::FlowFragment> lineFragments;
                    double used = 0.0;
                    while (index < fragments.size()) {
                        const double w = fragments[index].width;
                        if (used + w > availWidth && !lineFragments.empty()) break;
                        used += w;
                        lineFragments.push_back(fragments[index]);
                        ++index;
                    }
                    while (!lineFragments.empty() && lineFragments.back().text == " ") {
                        lineFragments.pop_back();
                    }
                    if (lineFragments.empty() && index >= fragments.size() && !firstLine) break;

                    ensureRoom(leading);
                    cursorY -= leading;

                    if (firstLine && heading) {
                        std::string title;
                        for (const InlineSpan& span : block.spans) title += span.text;
                        layout.bookmarks.push_back(
                            BookmarkIntent{title, block.headingLevel,
                                           static_cast<int>(layout.pages.size()) - 1,
                                           cursorY + size});
                    }
                    if (firstLine && !bullet.empty()) {
                        layout.pages.back().runs.push_back(PlacedRun{
                            bullet, StandardFont::Helvetica, size,
                            textLeft - options.listIndentPt * 0.75, cursorY});
                    }

                    double x = textLeft;
                    for (const detail::FlowFragment& fragment : lineFragments) {
                        if (fragment.text != " ") {
                            layout.pages.back().runs.push_back(PlacedRun{
                                fragment.text, fragment.font, fragment.fontSize, x, cursorY});
                        }
                        x += fragment.width;
                    }
                    firstLine = false;
                    if (index >= fragments.size()) break;
                }
                break;
            }
        }
    }

    layout.ok = true;
    return layout;
}

// ---------------------------------------------------------------------------
// 從 CSV 建立 PDF（PRD-IO-013 的 CSV 部分）
// ---------------------------------------------------------------------------
//
// Email（.eml）匯入不在這裡：解析 MIME 結構、多部分內容、附件抽取是另一件
// 與版面計算完全無關的事，做半套（例如只認得最簡單的單一 text/plain 部分）
// 會讓使用者以為一般的 Outlook / Gmail 匯出檔都能用，實際上多數會失敗且
// 失敗原因不明顯。本次只做 CSV，Email 明確不做（見 WP35 報告）。
//
// 欄寬與跨頁表頭是這個功能唯一容易「看起來能動但其實漏算」的地方：
//
//   - 欄寬先按內容自然寬度（含表頭）算，超過 maxColumnWidthPt 的欄位夾住；
//     若所有欄位夾住後總寬仍超過版心，等比例縮小到剛好塞進版心，
//     但不低於 minColumnWidthPt——太窄的欄位不如明確截斷內容
//   - 超長儲存格**截斷加刪節號**，不做欄內自動換行。理由是換行需要決定
//     「這一列因此變高，下一欄要不要對齊到同一個新高度」，那是一個完整的
//     表格版面引擎才該做的事；PRD 把 CSV 匯入列為 C（有餘力才做），
//     截斷是誠實地把限制講清楚，而不是做一半的換行（例如只有最後一欄
//     換行、其餘欄不換，導致同一列高度不一致、看起來像是渲染錯誤）
//   - 表頭在 firstRowIsHeader 為真時，每一頁都重新畫一次；這也是分頁時
//     每頁可容納的本文列數要扣掉表頭高度的原因

struct CsvImportOptions {
    PaperSize paper{kPaperA4};
    double marginPt{36.0};  // 0.5 吋，表格通常比純文字更需要善用版面寬度
    double fontSize{9.0};
    double leadingRatio{1.3};
    double cellPaddingPt{4.0};

    bool firstRowIsHeader{true};
    bool monospace{false};

    double minColumnWidthPt{24.0};
    double maxColumnWidthPt{200.0};

    [[nodiscard]] bool valid() const noexcept {
        return paper.valid() && fontSize > 0.0 && leadingRatio > 0.0 && cellPaddingPt >= 0.0 &&
               minColumnWidthPt > 0.0 && maxColumnWidthPt >= minColumnWidthPt;
    }
};

struct CsvPage {
    std::vector<std::string> bodyRowsFlat;  // 保留供除錯用；實際繪製走 rows
    std::vector<std::vector<std::string>> rows;  // 已截斷、已跳過表頭的本文列
};

struct CsvTableLayout {
    bool ok{false};
    std::string diagnostic;

    std::vector<std::string> headerRow;   // 空代表沒有表頭
    std::vector<double> columnWidthsPt;   // 含格內留白
    double rowHeightPt{0.0};

    StandardFont font{StandardFont::Helvetica};
    double fontSize{9.0};
    double leftX{0.0};
    double topY{0.0};
    double pageWidthPt{0.0};
    double pageHeightPt{0.0};

    std::vector<CsvPage> pages;

    [[nodiscard]] std::size_t pageCount() const noexcept { return pages.size(); }
    [[nodiscard]] std::size_t columnCount() const noexcept { return columnWidthsPt.size(); }
};

namespace detail {

// 截斷到指定寬度並在需要時補上刪節號。刪節號本身也佔寬度，
// 因此要邊減字邊重量，不能先截好長度再补三個點——那樣容易還是超寬。
[[nodiscard]] inline std::string truncateToWidth(const std::string& text, StandardFont font,
                                                 double fontSize, double maxWidth) {
    if (measureText(font, text, fontSize) <= maxWidth) return text;
    const std::string ellipsis = "...";
    const double ellipsisWidth = measureText(font, ellipsis, fontSize);
    if (ellipsisWidth > maxWidth) return {};  // 欄位窄到連刪節號都放不下

    std::string truncated;
    for (const char c : text) {
        std::string candidate = truncated;
        candidate.push_back(c);
        if (measureText(font, candidate + ellipsis, fontSize) > maxWidth) break;
        truncated = std::move(candidate);
    }
    return truncated + ellipsis;
}

}  // namespace detail

[[nodiscard]] inline CsvTableLayout layoutCsvTable(const std::string& csvText,
                                                   const CsvImportOptions& options = {}) {
    CsvTableLayout layout;
    if (!options.valid()) {
        layout.diagnostic = "CSV 匯入設定不合法";
        return layout;
    }
    const AsciiScan scan = scanAscii(csvText);
    if (!scan.ok) {
        layout.diagnostic = scan.describe();
        return layout;
    }

    std::vector<std::vector<std::string>> rows = parseCsvDocument(csvText);
    while (!rows.empty() && rows.back().size() == 1 && rows.back()[0].empty()) rows.pop_back();
    if (rows.empty()) {
        layout.diagnostic = "CSV 內容為空";
        return layout;
    }

    std::size_t columnCount = 0;
    for (const auto& row : rows) columnCount = std::max(columnCount, row.size());
    if (columnCount == 0) {
        layout.diagnostic = "CSV 沒有任何欄位";
        return layout;
    }

    layout.font = options.monospace ? StandardFont::Courier : StandardFont::Helvetica;
    layout.fontSize = options.fontSize;
    const PaperSize paper = options.paper;
    layout.pageWidthPt = paper.widthPt;
    layout.pageHeightPt = paper.heightPt;
    layout.leftX = options.marginPt;
    layout.topY = paper.heightPt - options.marginPt;

    // 自然欄寬：表頭與所有本文列裡最寬的那個字串，含左右各一份留白，
    // 夾在 [minColumnWidthPt, maxColumnWidthPt] 之間。
    std::vector<double> natural(columnCount, options.minColumnWidthPt);
    for (const auto& row : rows) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            const double w =
                measureText(layout.font, row[c], options.fontSize) + options.cellPaddingPt * 2.0;
            natural[c] = std::clamp(std::max(natural[c], w), options.minColumnWidthPt,
                                    options.maxColumnWidthPt);
        }
    }

    const double availableWidth = paper.widthPt - options.marginPt * 2.0;
    double naturalTotal = 0.0;
    for (const double w : natural) naturalTotal += w;
    if (naturalTotal <= 0.0) {
        layout.diagnostic = "欄寬計算結果為零";
        return layout;
    }

    // 等比縮小到塞進版心，但每欄仍不低於 minColumnWidthPt——縮到底之後
    // 若還是超出版心，那是欄數太多，這裡誠實地讓文件變寬印不下，
    // 不偷偷再犧牲可讀性去無限縮小字看不見的欄位。
    double scale = naturalTotal > availableWidth ? availableWidth / naturalTotal : 1.0;
    layout.columnWidthsPt.resize(columnCount);
    for (std::size_t c = 0; c < columnCount; ++c) {
        layout.columnWidthsPt[c] = std::max(options.minColumnWidthPt, natural[c] * scale);
    }

    layout.rowHeightPt = options.fontSize * options.leadingRatio + options.cellPaddingPt * 2.0;

    std::size_t startIndex = 0;
    if (options.firstRowIsHeader) {
        layout.headerRow = rows.front();
        layout.headerRow.resize(columnCount);
        startIndex = 1;
    }

    auto truncateRow = [&](const std::vector<std::string>& row) {
        std::vector<std::string> out(columnCount);
        for (std::size_t c = 0; c < columnCount; ++c) {
            const std::string cell = c < row.size() ? row[c] : std::string{};
            out[c] = detail::truncateToWidth(cell, layout.font, options.fontSize,
                                             layout.columnWidthsPt[c] - options.cellPaddingPt * 2.0);
        }
        return out;
    };

    if (options.firstRowIsHeader) {
        layout.headerRow = truncateRow(layout.headerRow);
    }

    const double contentHeight = paper.heightPt - options.marginPt * 2.0;
    const double headerHeight = options.firstRowIsHeader ? layout.rowHeightPt : 0.0;
    const auto rowsPerPage = static_cast<std::size_t>(
        std::max(1.0, std::floor((contentHeight - headerHeight) / layout.rowHeightPt)));

    for (std::size_t i = startIndex; i < rows.size(); i += rowsPerPage) {
        CsvPage page;
        const std::size_t end = std::min(rows.size(), i + rowsPerPage);
        for (std::size_t r = i; r < end; ++r) page.rows.push_back(truncateRow(rows[r]));
        layout.pages.push_back(std::move(page));
    }
    if (layout.pages.empty()) layout.pages.emplace_back();

    layout.ok = true;
    return layout;
}

// ---------------------------------------------------------------------------
// 從 URL 開啟（PRD-IO-010）
// ---------------------------------------------------------------------------

// 拒絕理由是列舉而不是字串：呼叫端要據此決定「顯示錯誤」還是「詢問使用者
// 是否仍要開啟」，用字串比對做那個決定遲早會因為文案調整而失效。
enum class UrlRejection {
    None,
    EmptyUrl,
    MalformedUrl,
    UnsupportedScheme,  // file:// data:// ftp:// 一律擋掉
    HttpNotAllowed,
    HostMissing,
    DeclaredSizeTooLarge,
    BodyTooLarge,
    ContentTypeMismatch,
    EmptyBody,
    NotPdfSignature,
    TransportFailed,
    HttpStatusNotOk,
};

[[nodiscard]] inline const char* describeUrlRejection(UrlRejection reason) noexcept {
    switch (reason) {
        case UrlRejection::None:
            return "";
        case UrlRejection::EmptyUrl:
            return "網址是空的";
        case UrlRejection::MalformedUrl:
            return "網址格式不正確";
        case UrlRejection::UnsupportedScheme:
            return "只允許 http 與 https；file、data、ftp 等結構描述一律拒絕";
        case UrlRejection::HttpNotAllowed:
            return "設定不允許未加密的 http";
        case UrlRejection::HostMissing:
            return "網址缺少主機名稱";
        case UrlRejection::DeclaredSizeTooLarge:
            return "伺服器宣告的長度超過上限";
        case UrlRejection::BodyTooLarge:
            return "下載內容超過上限";
        case UrlRejection::ContentTypeMismatch:
            return "內容型別不是 PDF";
        case UrlRejection::EmptyBody:
            return "下載內容是空的";
        case UrlRejection::NotPdfSignature:
            return "內容開頭不是 %PDF-";
        case UrlRejection::TransportFailed:
            return "網路傳輸失敗";
        case UrlRejection::HttpStatusNotOk:
            return "HTTP 狀態碼不是 200";
    }
    return "未知的拒絕原因";
}

struct UrlFetchPolicy {
    // 上限存在的理由不是磁碟空間，是「PDF 視為不可信任輸入」：一個回應
    // 無上限的下載迴圈本身就是可被利用的資源耗盡途徑。
    std::uint64_t maxBytes{256ull * 1024ull * 1024ull};
    bool allowPlainHttp{true};
    bool requirePdfContentType{true};
    bool requirePdfSignature{true};
};

struct UrlParts {
    bool ok{false};
    std::string scheme;  // 已轉小寫
    std::string host;
    std::string path;
};

[[nodiscard]] inline UrlParts splitUrl(std::string_view url) {
    UrlParts parts;
    const std::size_t colon = url.find("://");
    if (colon == std::string_view::npos || colon == 0) return parts;
    for (const char c : url.substr(0, colon)) {
        parts.scheme.push_back(
            static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    const std::string_view rest = url.substr(colon + 3);
    const std::size_t slash = rest.find('/');
    parts.host = std::string(slash == std::string_view::npos ? rest : rest.substr(0, slash));
    parts.path = slash == std::string_view::npos ? std::string("/") : std::string(rest.substr(slash));
    parts.ok = true;
    return parts;
}

[[nodiscard]] inline UrlRejection validateUrl(std::string_view url, const UrlFetchPolicy& policy) {
    if (url.empty()) return UrlRejection::EmptyUrl;
    const UrlParts parts = splitUrl(url);
    if (!parts.ok) return UrlRejection::MalformedUrl;
    if (parts.scheme != "http" && parts.scheme != "https") return UrlRejection::UnsupportedScheme;
    if (parts.scheme == "http" && !policy.allowPlainHttp) return UrlRejection::HttpNotAllowed;
    if (parts.host.empty()) return UrlRejection::HostMissing;
    return UrlRejection::None;
}

// Content-Type 可能帶參數（"application/pdf; charset=binary"），因此比對的是
// 分號之前的那一段。空字串在 requirePdfContentType 為 false 時放行。
[[nodiscard]] inline UrlRejection validateContentType(std::string_view contentType,
                                                      const UrlFetchPolicy& policy) {
    if (!policy.requirePdfContentType) return UrlRejection::None;
    std::string_view head = contentType.substr(0, contentType.find(';'));
    while (!head.empty() && (head.front() == ' ' || head.front() == '\t')) head.remove_prefix(1);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.remove_suffix(1);
    std::string lowered;
    lowered.reserve(head.size());
    for (const char c : head) {
        lowered.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    if (lowered == "application/pdf" || lowered == "application/x-pdf" ||
        lowered == "application/octet-stream") {
        // octet-stream 放行是務實的取捨：不少伺服器對 PDF 就是回這個。
        // 真正的把關是後面的 %PDF- 簽章檢查，那個沒得混。
        return UrlRejection::None;
    }
    return UrlRejection::ContentTypeMismatch;
}

[[nodiscard]] inline UrlRejection validateBody(const std::uint8_t* data, std::size_t size,
                                               const UrlFetchPolicy& policy) {
    if (size == 0) return UrlRejection::EmptyBody;
    if (static_cast<std::uint64_t>(size) > policy.maxBytes) return UrlRejection::BodyTooLarge;
    if (policy.requirePdfSignature) {
        if (size < 5 || data == nullptr) return UrlRejection::NotPdfSignature;
        // 部分伺服器會在 %PDF- 前面塞 BOM 或空白，規格也允許前置垃圾，
        // 但只在很前面找：整份掃描等於接受任何檔案裡剛好含有這五個位元組。
        const std::size_t window = std::min<std::size_t>(size - 4, 1024);
        for (std::size_t i = 0; i < window; ++i) {
            if (std::memcmp(data + i, "%PDF-", 5) == 0) return UrlRejection::None;
        }
        return UrlRejection::NotPdfSignature;
    }
    return UrlRejection::None;
}

// ---------------------------------------------------------------------------
// 從網頁 URL 建立 PDF（PRD-IO-014，降級為純文字擷取）
// ---------------------------------------------------------------------------
//
// **這是一個明確降級的功能，不是完整實作**——完整的「網頁轉 PDF」需要
// 一套 HTML/CSS 排版引擎（含框模型、層疊樣式、字型度量、圖片解碼、
// 甚至 JavaScript 動態內容）。專案的引擎級元件上限固定為 Qt 6 + PDFium +
// OpenSSL 三件（CLAUDE.md、PRD §4.1），Qt WebEngine 會整包引入 Chromium
// （數百 MB，含完整瀏覽器引擎與可執行 JavaScript 的沙盒），這既超出安裝包
// 130 MB 的上限，也與「不執行任何不可信程式碼」的安全立場正面衝突
// （PRD §8.2、CLAUDE.md「PDF 視為不可信任輸入」——網頁比 PDF 更不可信任）。
// 這個結論已回報為 BLOCKED，見 WP35 交付報告。
//
// 因此本節做的是誠實地縮小範圍：下載網頁（與 PRD-IO-010 共用同一套傳輸
// 注入、URL 驗證、大小上限機制）、抽出 <title> 與去除標籤後的純文字，
// 再交給既有的 layoutPlainText 排版。輸出是一份「網頁的文字內容」而不是
// 「網頁的畫面」——沒有版面、沒有圖片、沒有連結、沒有表格框線、沒有
// CSS 決定的顏色或字重。這對「留存一份網頁文字紀錄」的使用情境有用，
// 對「把網頁列印成好看的 PDF」這個使用者可能期待的情境沒用，
// 必須在 UI 上把這個落差講清楚，不能只在程式碼註解裡講。

enum class HtmlRejection {
    None,
    EmptyUrl,
    MalformedUrl,
    UnsupportedScheme,
    HttpNotAllowed,
    HostMissing,
    DeclaredSizeTooLarge,
    BodyTooLarge,
    ContentTypeMismatch,
    EmptyBody,
    TransportFailed,
    HttpStatusNotOk,
    NoExtractableText,  // 去除標籤與空白後沒有剩下任何內容
};

[[nodiscard]] inline const char* describeHtmlRejection(HtmlRejection reason) noexcept {
    switch (reason) {
        case HtmlRejection::None: return "";
        case HtmlRejection::EmptyUrl: return "網址是空的";
        case HtmlRejection::MalformedUrl: return "網址格式不正確";
        case HtmlRejection::UnsupportedScheme:
            return "只允許 http 與 https；file、data、ftp 等結構描述一律拒絕";
        case HtmlRejection::HttpNotAllowed: return "設定不允許未加密的 http";
        case HtmlRejection::HostMissing: return "網址缺少主機名稱";
        case HtmlRejection::DeclaredSizeTooLarge: return "伺服器宣告的長度超過上限";
        case HtmlRejection::BodyTooLarge: return "下載內容超過上限";
        case HtmlRejection::ContentTypeMismatch: return "內容型別不是網頁（text/html）";
        case HtmlRejection::EmptyBody: return "下載內容是空的";
        case HtmlRejection::TransportFailed: return "網路傳輸失敗";
        case HtmlRejection::HttpStatusNotOk: return "HTTP 狀態碼不是 200";
        case HtmlRejection::NoExtractableText:
            return "去除標籤後沒有可擷取的文字內容";
    }
    return "未知的拒絕原因";
}

struct HtmlFetchPolicy {
    std::uint64_t maxBytes{32ull * 1024ull * 1024ull};  // 網頁本文遠小於 PDF 的常見上限
    bool allowPlainHttp{true};
    bool requireHtmlContentType{true};
};

[[nodiscard]] inline HtmlRejection validateHtmlUrl(std::string_view url,
                                                   const HtmlFetchPolicy& policy) {
    if (url.empty()) return HtmlRejection::EmptyUrl;
    const UrlParts parts = splitUrl(url);
    if (!parts.ok) return HtmlRejection::MalformedUrl;
    if (parts.scheme != "http" && parts.scheme != "https") return HtmlRejection::UnsupportedScheme;
    if (parts.scheme == "http" && !policy.allowPlainHttp) return HtmlRejection::HttpNotAllowed;
    if (parts.host.empty()) return HtmlRejection::HostMissing;
    return HtmlRejection::None;
}

[[nodiscard]] inline HtmlRejection validateHtmlContentType(std::string_view contentType,
                                                           const HtmlFetchPolicy& policy) {
    if (!policy.requireHtmlContentType) return HtmlRejection::None;
    std::string_view head = contentType.substr(0, contentType.find(';'));
    while (!head.empty() && (head.front() == ' ' || head.front() == '\t')) head.remove_prefix(1);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.remove_suffix(1);
    std::string lowered;
    lowered.reserve(head.size());
    for (const char c : head) {
        lowered.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    if (lowered == "text/html" || lowered == "application/xhtml+xml") return HtmlRejection::None;
    return HtmlRejection::ContentTypeMismatch;
}

struct HtmlExtraction {
    bool ok{false};
    std::string diagnostic;
    std::string title;
    // 已去標籤、已解碼常見實體（含數字 entity，輸出 UTF-8）、空白已折疊。
    // 字元是否畫得出來由下游的 createPdfFromPlainText 判定，這裡不先過濾。
    std::string bodyText;
};

namespace detail {

[[nodiscard]] inline std::string lowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    return out;
}

// 移除某個標籤（含內容），例如 <script>...</script>、<style>...</style>。
// 這兩種標籤裡的文字不是「網頁內容」，畫進 PDF 只會產生一堆看不懂的
// JS/CSS 原始碼，比不擷取更誤導。
inline void stripTagWithContent(std::string& html, const std::string& tagLower) {
    const std::string openNeedle = "<" + tagLower;
    const std::string closeNeedle = "</" + tagLower + ">";
    std::string lowered = lowerAscii(html);
    std::size_t searchFrom = 0;
    while (true) {
        const std::size_t open = lowered.find(openNeedle, searchFrom);
        if (open == std::string::npos) break;
        const std::size_t openEnd = lowered.find('>', open);
        if (openEnd == std::string::npos) break;
        const std::size_t close = lowered.find(closeNeedle, openEnd);
        const std::size_t removeEnd =
            close == std::string::npos ? html.size() : close + closeNeedle.size();
        html.erase(open, removeEnd - open);
        lowered.erase(open, removeEnd - open);
        searchFrom = open;
    }
}

// 把一個 Unicode scalar value 編成 UTF-8 附加到 out。呼叫端必須先確認
// 它是合法 scalar（非 surrogate、≤ U+10FFFF）。
inline void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 解析數字 entity 的數值部分。自己走一遍字元而不用 strtol，是因為 strtol
// 會接受前導空白與正負號（`&#+65;`、`&# 65;` 都不是合法 entity），而且它的
// 溢位行為要另外查 errno；這裡的規則只有一條：整段必須都是該進位的數字。
[[nodiscard]] inline std::optional<char32_t> decodeNumericEntity(std::string_view numberPart,
                                                                 bool hexEntity) {
    if (numberPart.empty()) return std::nullopt;
    unsigned long long value = 0;
    for (const char c : numberPart) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (hexEntity && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (hexEntity && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) return std::nullopt;  // 尾端垃圾，例如 `&#12x3;`
        value = value * (hexEntity ? 16ULL : 10ULL) + static_cast<unsigned long long>(digit);
        if (value > 0x10FFFFULL) return std::nullopt;  // 提早停，長數字不會繞回
    }
    if (value == 0) return std::nullopt;                       // NUL 不是可顯示內容
    if (value >= 0xD800 && value <= 0xDFFF) return std::nullopt;  // surrogate 不是 scalar value
    return static_cast<char32_t>(value);
}

[[nodiscard]] inline std::string decodeEntities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') { out.push_back(text[i]); ++i; continue; }
        const std::size_t semicolon = text.find(';', i);
        if (semicolon == std::string_view::npos || semicolon - i > 10) {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        const std::string_view entity = text.substr(i + 1, semicolon - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity == "nbsp") out += ' ';
        else if (!entity.empty() && entity.front() == '#') {
            // 數字實體。`&#20013;` 與直接寫 UTF-8 的「中」是同一個字元的兩種
            // 合法寫法，所以這裡把 scalar value 編成 UTF-8，交給下游的字型
            // 涵蓋率檢查決定畫不畫得出來——不是在這裡先判死。
            // 不合法者（surrogate、超出 U+10FFFF、空數字、尾端垃圾）才標記成
            // 0xFF：那是刻意的無效 UTF-8，讓 createPdfFromPlainText 明確失敗，
            // 而不是靜默吐出一個看起來沒問題但缺字的 PDF。
            const std::string_view digits = entity.substr(1);
            const bool hexEntity = !digits.empty() && (digits.front() == 'x' || digits.front() == 'X');
            const std::string_view numberPart = hexEntity ? digits.substr(1) : digits;
            const std::optional<char32_t> scalar = decodeNumericEntity(numberPart, hexEntity);
            if (scalar) {
                appendUtf8(out, *scalar);
            } else {
                out.push_back(static_cast<char>(0xFF));  // 明確標記為不可表示，後續驗證會擋下
            }
        } else {
            // 未知具名實體：原樣保留（含 & 與 ;），比猜測性刪除更不容易
            // 誤刪使用者實際想看到的內容（例如某些少見但合法的實體）。
            out.push_back('&');
            out += entity;
            out += ';';
            i = semicolon + 1;
            continue;
        }
        i = semicolon + 1;
    }
    return out;
}

}  // namespace detail

[[nodiscard]] inline HtmlExtraction extractReadableText(const std::string& html) {
    HtmlExtraction result;
    std::string work = html;

    detail::stripTagWithContent(work, "script");
    detail::stripTagWithContent(work, "style");
    detail::stripTagWithContent(work, "noscript");
    detail::stripTagWithContent(work, "head");  // <title> 另外抽取，見下方，抽完才移除 <head>

    // <title>：在移除 <head> 之前先抓。
    {
        const std::string lowered = detail::lowerAscii(html);
        const std::size_t open = lowered.find("<title");
        if (open != std::string::npos) {
            const std::size_t openEnd = lowered.find('>', open);
            const std::size_t close = lowered.find("</title>", openEnd == std::string::npos ? open : openEnd);
            if (openEnd != std::string::npos && close != std::string::npos && close > openEnd) {
                result.title = detail::decodeEntities(html.substr(openEnd + 1, close - openEnd - 1));
            }
        }
    }

    // 區塊級標籤結束時換行，避免整頁文字黏成一行。這份清單刻意只覆蓋
    // 最常見的區塊元素，不追求完整的 HTML5 內容模型分類。
    static const std::array<std::string_view, 9> kBlockCloseTags = {
        "</p>", "</div>", "</li>", "</h1>", "</h2>", "</h3>", "</h4>", "</tr>", "</table>"};
    std::string withBreaks = work;
    for (const std::string_view tag : kBlockCloseTags) {
        std::string lowered = detail::lowerAscii(withBreaks);
        std::size_t pos = 0;
        std::string rebuilt;
        rebuilt.reserve(withBreaks.size());
        std::size_t last = 0;
        while ((pos = lowered.find(tag, pos)) != std::string::npos) {
            rebuilt.append(withBreaks, last, pos + tag.size() - last);
            rebuilt.push_back('\n');
            pos += tag.size();
            last = pos;
        }
        rebuilt.append(withBreaks, last, withBreaks.size() - last);
        withBreaks = std::move(rebuilt);
    }
    // <br> 系列（<br>、<br/>、<br />）換行。
    {
        std::string lowered = detail::lowerAscii(withBreaks);
        std::string rebuilt;
        rebuilt.reserve(withBreaks.size());
        std::size_t i = 0;
        while (i < withBreaks.size()) {
            if (lowered.compare(i, 3, "<br") == 0) {
                const std::size_t end = lowered.find('>', i);
                if (end != std::string::npos) {
                    rebuilt.push_back('\n');
                    i = end + 1;
                    continue;
                }
            }
            rebuilt.push_back(withBreaks[i]);
            ++i;
        }
        withBreaks = std::move(rebuilt);
    }

    // 去除所有剩餘標籤：從 '<' 到最近的 '>'。這不是一個真正的 HTML 剖析器
    // （不處理屬性值裡含 '>' 的邊緣情況，例如 title="a>b"），但對絕大多數
    // 真實網頁已經足夠，且錯誤的後果只是多幾個字元被誤刪，不是安全問題。
    std::string stripped;
    stripped.reserve(withBreaks.size());
    bool inTag = false;
    for (const char c : withBreaks) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) stripped.push_back(c);
    }

    std::string decoded = detail::decodeEntities(stripped);

    // 壓縮多餘的空白：連續空白（含換行）收成單一分隔，但保留段落換行——
    // 用「一行內有非空白內容才輸出」的規則做到這件事，而不是逐字元判斷。
    std::vector<std::string> lines;
    std::string current;
    for (const char c : decoded) {
        if (c == '\n' || c == '\r') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(current);

    std::string finalText;
    bool previousBlank = true;  // 開頭不留空行
    for (std::string& line : lines) {
        std::size_t begin = line.find_first_not_of(" \t");
        std::size_t last = line.find_last_not_of(" \t");
        std::string trimmed =
            begin == std::string::npos ? std::string{} : line.substr(begin, last - begin + 1);
        // 折疊行內連續空白。
        std::string collapsed;
        bool lastWasSpace = false;
        for (const char c : trimmed) {
            if (c == ' ' || c == '\t') {
                if (!lastWasSpace) collapsed.push_back(' ');
                lastWasSpace = true;
            } else {
                collapsed.push_back(c);
                lastWasSpace = false;
            }
        }
        if (collapsed.empty()) {
            if (!previousBlank) finalText += '\n';
            previousBlank = true;
        } else {
            finalText += collapsed;
            finalText += '\n';
            previousBlank = false;
        }
    }
    while (!finalText.empty() && finalText.back() == '\n') finalText.pop_back();

    if (finalText.empty()) {
        result.diagnostic = describeHtmlRejection(HtmlRejection::NoExtractableText);
        return result;
    }

    result.bodyText = finalText;
    result.ok = true;
    return result;
}

}  // namespace alioth::domain::create
