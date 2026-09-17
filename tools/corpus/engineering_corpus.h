#pragma once

// 工程圖合成語料產生器（WBS 7.2 / 7.4，PRD §9「自建工程圖語料」）。
//
// 為什麼要重做語料：原本 alioth_bench --generate 每頁只畫 24 個矩形，
// 單張圖磚 0.4 毫秒就渲染完。那個數字量到的是排程與快取的成本，
// 不是 PDFium 解析內容串流的成本——而真實工程圖的成本幾乎全在後者。
// 用它宣告 PRD §8.1 達標等於用空白頁證明印表機很快。
// sprint/archive/sprint-0-status.md 已明確標註舊數字不可用於驗收。
//
// 本產生器刻意逼近 CAD 匯出的 PDF 特徵：
//   - A0 幅面（2384 × 3370 點），而不是 A4
//   - 每頁數千條折線，線寬分佈在 0.13–2.0 點之間（ISO 128 的線寬階梯）
//   - 虛線／點劃線（中心線、隱藏線）佔一定比例，會觸發 PDFium 的 dash 路徑
//   - 圓與圓弧以三次貝茲近似，觸發曲線細分而不只是直線光柵化
//   - 剖面線（密集平行細線）——CAD 匯出最常見的病態圖樣
//   - 數百個尺寸標註文字物件
//   - 可選的嵌入影像（掃描套圖）
//
// 決定性輸出是硬性要求：基準影像回歸（WBS 7.3）要能逐位元組重現同一份語料，
// 因此亂數用自帶的 xorshift 而不是 <random>（實作因標準庫版本而異），
// 浮點格式化用整數運算而不是 printf（printf 受 locale 影響，小數點可能變成逗號）。
//
// 檔案大小的取捨：variantCount 個不同的內容串流在頁面之間輪流共用。
// 500 頁各自獨立會產生約 200 MB 的檔案，光是寫檔就要數秒，
// 讓「冷啟動」這項指標量到的變成磁碟頻寬。輪流共用讓 PDFium 仍需逐頁解析
// （它不快取跨頁的內容串流解析結果），但檔案維持在數 MB。
// 需要極端情境時把 variantCount 設成 pageCount 即可。

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace alioth::corpus {

// A0 直放（ISO 216：841 × 1189 mm）。
inline constexpr double kA0WidthPt = 2384.0;
inline constexpr double kA0HeightPt = 3370.0;

struct CorpusOptions {
    int pageCount{8};
    double pageWidth{kA0WidthPt};
    double pageHeight{kA0HeightPt};

    // 每頁的折線數。每條折線含 2–6 個線段，因此實際線段數約為本值的 3–4 倍。
    int polylines{2400};
    // 每頁的尺寸標註／註記文字物件數。
    int textObjects{320};
    // 每頁的剖面線區塊數，每塊含 40 條密集平行線。
    int hatchBlocks{12};
    // 每頁的圓／圓弧數。
    int circles{160};

    // 不同內容串流的數量（見檔頭關於檔案大小的取捨）。
    int variantCount{8};

    // 嵌入未壓縮 RGB 影像（模擬掃描套圖）。預設關閉：一張 512×512 的原始 RGB
    // 就是 768 KB，開啟後檔案大小由影像主導，量到的會是 I/O 而不是渲染。
    bool embedImage{false};
    int imageEdgePixels{512};

    std::uint64_t seed{20260905ull};
};

namespace detail {

// xorshift64*。自帶亂數而非 <random>，理由見檔頭：語料必須逐位元組可重現。
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    // [0, bound) 的整數。
    int nextInt(int bound) noexcept {
        if (bound <= 0) return 0;
        return static_cast<int>(next() % static_cast<std::uint64_t>(bound));
    }

    // [0, 1) 的浮點，以整數除法產生，不同平台結果一致。
    double nextUnit() noexcept {
        return static_cast<double>(next() >> 11) / 9007199254740992.0;  // 2^53
    }

    double nextRange(double low, double high) noexcept {
        return low + (high - low) * nextUnit();
    }

private:
    std::uint64_t state_;
};

// 固定兩位小數的浮點格式化。
//
// 不用 std::to_string / printf：兩者都經過 C locale，德文或法文環境下小數點會變成逗號，
// 產出的 PDF 內容串流當場失效。這種錯誤只在特定機器上出現，是最難追的一類。
inline void appendNumber(std::string& out, double value) {
    if (!std::isfinite(value)) value = 0.0;
    bool negative = value < 0.0;
    if (negative) value = -value;

    auto scaled = static_cast<std::int64_t>(value * 100.0 + 0.5);
    const std::int64_t whole = scaled / 100;
    const std::int64_t frac = scaled % 100;

    if (negative && (whole != 0 || frac != 0)) out += '-';
    out += std::to_string(whole);
    if (frac != 0) {
        out += '.';
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) out += static_cast<char>('0' + frac % 10);
    }
}

inline void appendPoint(std::string& out, double x, double y) {
    appendNumber(out, x);
    out += ' ';
    appendNumber(out, y);
}

// ISO 128 的線寬階梯。CAD 匯出的 PDF 幾乎只用這幾個值。
inline constexpr double kLineWidths[] = {0.13, 0.18, 0.25, 0.35, 0.5, 0.7, 1.0, 1.4, 2.0};

// 中心線、隱藏線的虛線樣式。空字串代表實線。
inline constexpr const char* kDashPatterns[] = {
    "[] 0 d\n",           // 實線（輪廓線）
    "[] 0 d\n",
    "[] 0 d\n",
    "[6 3] 0 d\n",        // 隱藏線
    "[18 3 3 3] 0 d\n",   // 中心線
    "[2 2] 0 d\n",        // 尺寸界線
};

// 以四段三次貝茲近似圓。控制點係數 0.5523 是標準的圓近似常數。
inline void appendCircle(std::string& out, double cx, double cy, double r) {
    const double k = r * 0.5523;
    appendPoint(out, cx + r, cy);
    out += " m\n";
    appendPoint(out, cx + r, cy + k);
    out += ' ';
    appendPoint(out, cx + k, cy + r);
    out += ' ';
    appendPoint(out, cx, cy + r);
    out += " c\n";
    appendPoint(out, cx - k, cy + r);
    out += ' ';
    appendPoint(out, cx - r, cy + k);
    out += ' ';
    appendPoint(out, cx - r, cy);
    out += " c\n";
    appendPoint(out, cx - r, cy - k);
    out += ' ';
    appendPoint(out, cx - k, cy - r);
    out += ' ';
    appendPoint(out, cx, cy - r);
    out += " c\n";
    appendPoint(out, cx + k, cy - r);
    out += ' ';
    appendPoint(out, cx + r, cy - k);
    out += ' ';
    appendPoint(out, cx + r, cy);
    out += " c\nS\n";
}

// 圖框與標題欄。真實圖紙一定有，而且是最粗的線，對「縮放到全頁」的視覺回歸很有用。
inline void appendTitleBlock(std::string& out, double w, double h, int variant) {
    out += "0 0 0 RG\n2 w\n[] 0 d\n";
    out += "20 20 ";
    appendNumber(out, w - 40.0);
    out += ' ';
    appendNumber(out, h - 40.0);
    out += " re\nS\n";

    out += "1.4 w\n";
    appendNumber(out, w - 620.0);
    out += " 40 560 300 re\nS\n";
    for (int row = 1; row < 6; ++row) {
        const double y = 40.0 + row * 50.0;
        appendPoint(out, w - 620.0, y);
        out += " m\n";
        appendPoint(out, w - 60.0, y);
        out += " l\nS\n";
    }

    out += "BT\n/F1 24 Tf\n";
    appendPoint(out, w - 600.0, 300.0);
    out += " Td\n(ALIOTH SYNTHETIC DRAWING SHEET ";
    out += std::to_string(variant);
    out += ") Tj\nET\n";
}

// 剖面線：同方向的密集平行細線。CAD 匯出最常見、也最耗渲染的圖樣。
inline void appendHatch(std::string& out, Rng& rng, double w, double h) {
    const double x = rng.nextRange(60.0, w - 460.0);
    const double y = rng.nextRange(400.0, h - 260.0);
    const double bw = rng.nextRange(120.0, 400.0);
    const double bh = rng.nextRange(120.0, 340.0);
    const double step = rng.nextRange(3.0, 7.0);

    out += "0.13 w\n[] 0 d\n0.2 0.2 0.2 RG\n";
    for (double offset = 0.0; offset < bw + bh; offset += step) {
        const double x0 = x + offset;
        const double y0 = y;
        const double x1 = x + offset - bh;
        const double y1 = y + bh;
        appendPoint(out, x0 > x + bw ? x + bw : x0, y0);
        out += " m\n";
        appendPoint(out, x1 < x ? x : x1, y1);
        out += " l\nS\n";
    }
}

inline std::string makeContentStream(const CorpusOptions& options, int variant, bool withImage) {
    Rng rng(options.seed + static_cast<std::uint64_t>(variant) * 0x9E3779B97F4A7C15ull);
    const double w = options.pageWidth;
    const double h = options.pageHeight;

    std::string out;
    out.reserve(static_cast<std::size_t>(options.polylines) * 120 + 65536);

    if (withImage) {
        // 掃描套圖：鋪在圖框內側左上角，模擬「舊圖掃描後疊向量圖」的常見情境。
        out += "q\n";
        appendNumber(out, w * 0.4);
        out += " 0 0 ";
        appendNumber(out, w * 0.4);
        out += ' ';
        appendPoint(out, 80.0, h - w * 0.4 - 80.0);
        out += " cm\n/Im0 Do\nQ\n";
    }

    appendTitleBlock(out, w, h, variant);

    // 折線。每條各自設定線寬、虛線與顏色——真實 CAD 匯出正是這樣，
    // 圖形狀態切換的成本因此也被量進去。
    for (int i = 0; i < options.polylines; ++i) {
        const double width = kLineWidths[rng.nextInt(9)];
        appendNumber(out, width);
        out += " w\n";
        out += kDashPatterns[rng.nextInt(6)];

        const double grey = rng.nextUnit() * 0.35;
        appendNumber(out, grey);
        out += ' ';
        appendNumber(out, grey);
        out += ' ';
        appendNumber(out, grey);
        out += " RG\n";

        double x = rng.nextRange(30.0, w - 30.0);
        double y = rng.nextRange(30.0, h - 30.0);
        appendPoint(out, x, y);
        out += " m\n";

        const int vertices = 2 + rng.nextInt(5);
        for (int v = 0; v < vertices; ++v) {
            // 工程圖以正交與 45 度線為主，不是隨機散線。
            const int direction = rng.nextInt(4);
            const double length = rng.nextRange(20.0, 260.0);
            switch (direction) {
                case 0: x += length; break;
                case 1: y += length; break;
                case 2: x -= length; break;
                default: y -= length; break;
            }
            if (x < 30.0) x = 30.0;
            if (x > w - 30.0) x = w - 30.0;
            if (y < 30.0) y = 30.0;
            if (y > h - 30.0) y = h - 30.0;
            appendPoint(out, x, y);
            out += " l\n";
        }
        out += "S\n";
    }

    out += "[] 0 d\n0.35 w\n0 0 0 RG\n";
    for (int i = 0; i < options.circles; ++i) {
        appendCircle(out, rng.nextRange(60.0, w - 60.0), rng.nextRange(60.0, h - 60.0),
                     rng.nextRange(4.0, 90.0));
    }

    for (int i = 0; i < options.hatchBlocks; ++i) {
        appendHatch(out, rng, w, h);
    }

    // 尺寸標註。用標準字型 Helvetica，不嵌入字型檔——測試要能在乾淨機器上跑。
    // CJK 語料另計，見 status.md 的已知缺口。
    out += "0 0 0 rg\n";
    for (int i = 0; i < options.textObjects; ++i) {
        const int size = 6 + rng.nextInt(6);
        out += "BT\n/F1 ";
        out += std::to_string(size);
        out += " Tf\n";
        appendPoint(out, rng.nextRange(40.0, w - 200.0), rng.nextRange(40.0, h - 40.0));
        out += " Td\n(";
        // 尺寸文字：數字加公差，不含需跳脫的字元。
        out += std::to_string(10 + rng.nextInt(9990));
        out += '.';
        out += std::to_string(rng.nextInt(10));
        if (rng.nextInt(3) == 0) {
            out += " +/-0.";
            out += std::to_string(1 + rng.nextInt(9));
        }
        out += ") Tj\nET\n";
    }

    return out;
}

// 未壓縮 RGB 影像資料。刻意不壓縮：專案不引入 zlib（PRD §4.1 三件式相依），
// 而未壓縮同樣能觸發 PDFium 的影像取樣路徑，只是檔案較大。
inline std::string makeImageData(int edge, std::uint64_t seed) {
    Rng rng(seed);
    std::string data;
    data.resize(static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge) * 3);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        // 掃描件的特徵是灰階為主、帶雜訊，不是彩色噪點。
        const auto base = static_cast<unsigned char>(180 + rng.nextInt(70));
        data[i] = static_cast<char>(base);
        data[i + 1] = static_cast<char>(base);
        data[i + 2] = static_cast<char>(base);
    }
    return data;
}

}  // namespace detail

// 產生語料的位元組。輸出對相同的 CorpusOptions 逐位元組可重現。
inline std::string generate(const CorpusOptions& options) {
    const int pageCount = options.pageCount > 0 ? options.pageCount : 1;
    const int variantCount =
        options.variantCount > 0 ? (options.variantCount < pageCount ? options.variantCount
                                                                    : pageCount)
                                 : 1;

    std::vector<std::string> objects;

    // 物件編號配置（1 起算）：
    //   1 = Catalog、2 = Pages、3 = Font
    //   4 = Image（僅 embedImage 時）
    //   接著 variantCount 個內容串流，最後 pageCount 個頁面字典。
    const int fontObj = 3;
    const int imageObj = options.embedImage ? 4 : 0;
    const int firstContentObj = options.embedImage ? 5 : 4;
    const int firstPageObj = firstContentObj + variantCount;

    std::string kids;
    for (int i = 0; i < pageCount; ++i) {
        kids += std::to_string(firstPageObj + i) + " 0 R ";
    }
    if (!kids.empty()) kids.pop_back();

    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageCount) +
                      " >>");
    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

    if (options.embedImage) {
        const std::string pixels =
            detail::makeImageData(options.imageEdgePixels, options.seed ^ 0xA5A5A5A5ull);
        objects.push_back("<< /Type /XObject /Subtype /Image /Width " +
                          std::to_string(options.imageEdgePixels) + " /Height " +
                          std::to_string(options.imageEdgePixels) +
                          " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length " +
                          std::to_string(pixels.size()) + " >>\nstream\n" + pixels + "\nendstream");
    }

    for (int v = 0; v < variantCount; ++v) {
        const std::string content = detail::makeContentStream(options, v, options.embedImage);
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }

    std::string resources = "<< /Font << /F1 " + std::to_string(fontObj) + " 0 R >>";
    if (options.embedImage) {
        resources += " /XObject << /Im0 " + std::to_string(imageObj) + " 0 R >>";
    }
    resources += " >>";

    std::string mediaBox = "[0 0 ";
    detail::appendNumber(mediaBox, options.pageWidth);
    mediaBox += ' ';
    detail::appendNumber(mediaBox, options.pageHeight);
    mediaBox += ']';

    for (int i = 0; i < pageCount; ++i) {
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox " + mediaBox + " /Resources " +
                          resources + " /Contents " +
                          std::to_string(firstContentObj + (i % variantCount)) + " 0 R >>");
    }

    // 組檔。xref 偏移量自己算，順帶驗證我們對檔案結構的理解——
    // 這份語料自己也要通過 qpdf --check。
    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const std::size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

// 基準影像回歸專用的固定語料（WBS 7.3）。
//
// 參數釘死在這裡而不是散在各測試裡：基準影像一旦建立，語料的任何改動都會讓比對失敗。
// 把它集中成一個具名函式，改動時就必須明確地經過這裡，而不是不小心改到。
inline CorpusOptions goldenCorpusOptions() {
    CorpusOptions options;
    options.pageCount = 2;
    options.pageWidth = 1190.0;   // A3 橫放；基準影像要能整頁塞進合理大小的 PNG
    options.pageHeight = 842.0;
    options.polylines = 220;
    options.textObjects = 40;
    options.hatchBlocks = 3;
    options.circles = 24;
    options.variantCount = 2;
    options.embedImage = false;
    options.seed = 7300000ull;  // WBS 7.3
    return options;
}

}  // namespace alioth::corpus
