#include "app/print/stamp_content_stream.h"

#include <cmath>
#include <cstdio>

#include "engine/fonts/cjk_font_library.h"
#include "engine/fonts/text_runs.h"

namespace alioth::app::print {
namespace {

constexpr int kDecimals = 4;

}  // namespace

std::string formatStreamNumber(double value) {
    if (!std::isfinite(value)) return "0";

    // 自行做定點轉換，完全避開任何受地區設定影響的格式化路徑。
    const bool negative = value < 0.0;
    double magnitude = negative ? -value : value;

    double scale = 1.0;
    for (int i = 0; i < kDecimals; ++i) scale *= 10.0;
    long long scaled = static_cast<long long>(std::llround(magnitude * scale));

    std::string integerPart = std::to_string(scaled / static_cast<long long>(scale));
    long long fraction = scaled % static_cast<long long>(scale);

    std::string out;
    if (negative && scaled != 0) out.push_back('-');
    out += integerPart;

    if (fraction != 0) {
        std::string digits = std::to_string(fraction);
        digits.insert(digits.begin(), static_cast<std::size_t>(kDecimals) - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') digits.pop_back();
        out.push_back('.');
        out += digits;
    }
    return out;
}

std::string escapePdfLiteralString(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
            case '(':
            case ')':
                out.push_back('\\');
                out.push_back(ch);
                break;
            case '\r':
                out += "\\r";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

StampStream makeTextStampStream(const std::string& text, double xPt, double yPt,
                                const StampStreamOptions& options) {
    StampStream stream;
    if (text.empty()) {
        stream.diagnostic = "戳記文字為空";
        return stream;
    }
    if (options.fontResourceName.empty()) {
        stream.diagnostic = "未指定字型資源名稱";
        return stream;
    }
    if (!(options.fontSize > 0.0) || !std::isfinite(options.fontSize)) {
        stream.diagnostic = "字級必須為正數";
        return stream;
    }

    std::string out = "q\n";
    if (!options.extGStateName.empty()) {
        out += "/" + options.extGStateName + " gs\n";
    }
    out += formatStreamNumber(options.red) + " " + formatStreamNumber(options.green) + " " +
           formatStreamNumber(options.blue) + " rg\n";
    out += "BT\n";
    // 字型在下面逐段設定（拉丁與 CJK 各一個），這裡不先設一次——
    // 先設再被覆蓋只是多一行沒有作用的指令。
    // 用 Tm 而不是 Td：Td 相對於前一個文字物件的位置，串接多個戳記時
    // 偏移會累積，而 Tm 是絕對的，任何一則戳記算錯都不會污染下一則。
    out += "1 0 0 1 " + formatStreamNumber(xPt) + " " + formatStreamNumber(yPt) + " Tm\n";

    // 拉丁與 CJK 分段畫（ADR-007）。同一段裡兩者混用的話，不是中文變亂碼
    // 就是拉丁字被當成雙位元組讀掉——兩種都是「畫出來是別的東西」。
    for (const alioth::engine::fonts::TextRun& run : alioth::engine::fonts::splitTextRuns(text)) {
        if (run.cjk) {
            if (options.cjkFontResourceName.empty()) {
                stream.diagnostic = "戳記文字含 CJK，但未指定 CJK 字型資源名稱";
                return stream;
            }
            auto& library = alioth::engine::fonts::CjkFontLibrary::instance();
            if (!library.available()) {
                stream.diagnostic = "沒有可用的 CJK 字型，無法畫出這段戳記文字";
                return stream;
            }
            for (const char32_t codepoint : run.codepoints) {
                if (library.glyphFor(codepoint) == 0) {
                    // 缺一個字就整則失敗。輸出「只剩看得懂的那幾個字」的戳記
                    // 看起來像排版問題，不像功能失敗，使用者不會回報。
                    stream.diagnostic = "內嵌的 CJK 子集裡沒有這個字的字形";
                    return stream;
                }
            }
            out += "/" + options.cjkFontResourceName + " " +
                   formatStreamNumber(options.fontSize) + " Tf\n";
            // run.bytes 已是 Identity-H 編碼並跳脫過，不可再跳脫一次——
            // 二次跳脫會把反斜線本身變成資料，整組 GID 因此偏掉。
            out += "(" + run.bytes + ") Tj\n";
            stream.cjkCodepoints.insert(run.codepoints.begin(), run.codepoints.end());
        } else {
            out += "/" + options.fontResourceName + " " + formatStreamNumber(options.fontSize) +
                   " Tf\n";
            out += "(" + escapePdfLiteralString(run.bytes) + ") Tj\n";
        }
    }

    out += "ET\n";
    out += "Q\n";

    stream.valid = true;
    stream.content = std::move(out);
    return stream;
}

domain::PointF stampBaselineOrigin(double pageWidthPt, double pageHeightPt, double textWidthPt,
                                   double textHeightPt, StampAnchor anchor,
                                   const StampMargins& margins) {
    const double left = margins.left;
    const double right = pageWidthPt - margins.right;
    const double bottom = margins.bottom;
    const double top = pageHeightPt - margins.top;

    double x = left;
    switch (anchor) {
        case StampAnchor::TopLeft:
        case StampAnchor::MiddleLeft:
        case StampAnchor::BottomLeft:
            break;
        case StampAnchor::TopCenter:
        case StampAnchor::Center:
        case StampAnchor::BottomCenter:
            x = left + (right - left - textWidthPt) / 2.0;
            break;
        case StampAnchor::TopRight:
        case StampAnchor::MiddleRight:
        case StampAnchor::BottomRight:
            x = right - textWidthPt;
            break;
    }

    double y = bottom;
    switch (anchor) {
        case StampAnchor::TopLeft:
        case StampAnchor::TopCenter:
        case StampAnchor::TopRight:
            y = top - textHeightPt;
            break;
        case StampAnchor::MiddleLeft:
        case StampAnchor::Center:
        case StampAnchor::MiddleRight:
            y = bottom + (top - bottom - textHeightPt) / 2.0;
            break;
        case StampAnchor::BottomLeft:
        case StampAnchor::BottomCenter:
        case StampAnchor::BottomRight:
            break;
    }

    return domain::PointF{x, y};
}

}  // namespace alioth::app::print
