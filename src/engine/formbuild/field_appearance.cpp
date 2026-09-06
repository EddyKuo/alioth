#include "engine/formbuild/field_appearance.h"

#include "engine/fonts/text_runs.h"

#include "engine/fonts/cjk_font_library.h"

#include <algorithm>
#include <cmath>

#include "domain/barcode.h"
#include "engine/annotations/appearance_stream.h"
#include "engine/objects/pdf_object.h"

namespace alioth::engine::formbuild {

namespace {

using annotations::formatNumber;

// 自動字級的上下限。上限 12 是表單的慣用大小；下限 4 是「小到看不清但仍然
// 存在」的界線——再小就該讓欄位變高，而不是把字縮到看不見。
constexpr double kMaxAutoFontSize = 12.0;
constexpr double kMinAutoFontSize = 4.0;

// 文字與框線之間的內縮。與 Acrobat 的預設值一致，換成別的數字會讓
// 同一份表單在兩邊看起來錯開一兩個點。
constexpr double kTextPadding = 2.0;

[[nodiscard]] std::string num(double value) { return formatNumber(value); }

void appendColor(std::string& out, const FieldColor& color, bool stroke) {
    out += num(color.r) + " " + num(color.g) + " " + num(color.b) +
           (stroke ? " RG\n" : " rg\n");
}

void appendRect(std::string& out, double x, double y, double w, double h) {
    out += num(x) + " " + num(y) + " " + num(w) + " " + num(h) + " re\n";
}

// 背景與邊框。邊框的路徑要內縮半個線寬，否則描邊會有一半落在 /BBox 外面
// 被裁掉，視覺上變成「邊框比設定的細一半」。
void appendChrome(std::string& out, const FieldAppearanceCharacteristics& mk, double width,
                  double height) {
    if (mk.backgroundColor.has_value()) {
        appendColor(out, *mk.backgroundColor, false);
        appendRect(out, 0.0, 0.0, width, height);
        out += "f\n";
    }
    if (mk.borderColor.has_value() && mk.borderWidth > 0.0) {
        const double w = mk.borderWidth;
        appendColor(out, *mk.borderColor, true);
        out += num(w) + " w\n";
        if (mk.dashedBorder) out += "[" + num(w * 3.0) + "] 0 d\n";
        appendRect(out, w / 2.0, w / 2.0, width - w, height - w);
        out += "S\n";
    }
}

[[nodiscard]] double resolveFontSize(const FieldDefinition& definition, double height,
                                     int lineCount) {
    if (definition.fontSize > 0.0) return definition.fontSize;
    const double perLine = height / std::max(1, lineCount) - kTextPadding * 2.0;
    return std::clamp(perLine * 0.72, kMinAutoFontSize, kMaxAutoFontSize);
}

[[nodiscard]] double alignedX(const FieldDefinition& definition, const std::string& text,
                              double fontSize, double width) {
    const double textWidth = estimateHelveticaWidth(text, fontSize);
    switch (definition.alignment) {
        case FieldAlignment::Center:
            return std::max(kTextPadding, (width - textWidth) / 2.0);
        case FieldAlignment::Right:
            return std::max(kTextPadding, width - kTextPadding - textWidth);
        case FieldAlignment::Left:
            break;
    }
    return kTextPadding;
}

void appendTextRun(std::string& out, const std::string& asciiText, double x, double y,
                   double fontSize, const FieldColor& color,
                   std::set<char32_t>* cjkCodepoints = nullptr) {
    out += "BT\n";
    appendColor(out, color, false);
    out += num(x) + " " + num(y) + " Td\n";
    // 一段文字裡可能同時有拉丁與中文，兩者的字型與編碼都不同，必須分段畫。
    // 整段用同一個字型的話，不是中文變亂碼就是拉丁字被當成雙位元組讀掉。
    for (const fonts::TextRun& run : fonts::splitTextRuns(asciiText)) {
        if (run.cjk) {
            out += "/CJK " + num(fontSize) + " Tf\n";
            // run.bytes 已是 Identity-H 編碼並跳脫過，**不可再跳脫一次**——
            // 二次跳脫會把反斜線本身變成資料，整組 GID 因此偏掉。
            out += "(" + run.bytes + ") Tj\n";
            if (cjkCodepoints != nullptr) {
                cjkCodepoints->insert(run.codepoints.begin(), run.codepoints.end());
            }
        } else {
            out += "/Helv " + num(fontSize) + " Tf\n";
            // escapeLiteralString 只做跳脫，不含外層括號——序列化器才負責加括號。
            // 少了它產出的是語法錯誤的內容串流：PDFium 容忍並照樣顯示，qpdf 會報
            // 「EOF while reading token」。這種「我們自己看起來沒事」的缺陷正是
            // 外部檢查器的價值。
            out += "(" + objects::escapeLiteralString(run.bytes) + ") Tj\n";
        }
    }
    out += "ET\n";
}

[[nodiscard]] std::vector<std::string> splitLines(const std::string& text) {
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

// 核取記號用路徑而不是 ZapfDingbats 的字元。
//
// 走字型的話 /Resources 就得多帶一個字型物件，而勾選記號是固定形狀，
// 用兩段線就能畫出來且在所有檢視器上完全一致——沒有字型替換的變數。
[[nodiscard]] std::string checkMarkPath(double width, double height, const FieldColor& color) {
    const double size = std::min(width, height);
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    const double s = size * 0.30;

    std::string out;
    appendColor(out, color, true);
    out += num(std::max(0.8, size * 0.12)) + " w\n1 J\n1 j\n";
    out += num(cx - s) + " " + num(cy + s * 0.1) + " m\n";
    out += num(cx - s * 0.25) + " " + num(cy - s * 0.75) + " l\n";
    out += num(cx + s) + " " + num(cy + s * 0.8) + " l\nS\n";
    return out;
}

// 圓形以四段三次貝茲曲線逼近。0.5523 是單位圓的標準控制點係數；
// 用直線多邊形逼近在放大時會看得出稜角。
void appendCircle(std::string& out, double cx, double cy, double radius) {
    constexpr double kKappa = 0.5523;
    const double k = radius * kKappa;
    out += num(cx + radius) + " " + num(cy) + " m\n";
    out += num(cx + radius) + " " + num(cy + k) + " " + num(cx + k) + " " + num(cy + radius) +
           " " + num(cx) + " " + num(cy + radius) + " c\n";
    out += num(cx - k) + " " + num(cy + radius) + " " + num(cx - radius) + " " + num(cy + k) +
           " " + num(cx - radius) + " " + num(cy) + " c\n";
    out += num(cx - radius) + " " + num(cy - k) + " " + num(cx - k) + " " + num(cy - radius) +
           " " + num(cx) + " " + num(cy - radius) + " c\n";
    out += num(cx + k) + " " + num(cy - radius) + " " + num(cx + radius) + " " + num(cy - k) +
           " " + num(cx + radius) + " " + num(cy) + " c\n";
}

[[nodiscard]] FieldAppearance failure(std::string reason) {
    FieldAppearance out;
    out.diagnostic = std::move(reason);
    return out;
}

}  // namespace

AsciiFold foldToWinAnsi(const std::string& utf8) {
    // ADR-007 之後非 ASCII 走內嵌的 CJK 子集，不再一律拒絕。
    //
    // 但「畫不出來就要失敗」的原則沒有變，只是條件變了：絕不輸出只剩 ASCII
    // 的殘缺字串。那正是「/V 有值但畫面空白／殘缺」這個症狀的來源，
    // 而表單欄位出現這個症狀時使用者完全不會發現——欄位看起來就只是空的。
    for (const char32_t codepoint : fonts::decodeUtf8(utf8)) {
        if (codepoint == U'\n' || codepoint == U'\r') continue;
        if (codepoint >= 0x20 && codepoint < 0x7F) continue;
        if (codepoint < 128) return AsciiFold{false, {}};  // 控制字元

        auto& library = fonts::CjkFontLibrary::instance();
        if (!library.available() || library.glyphFor(codepoint) == 0) return AsciiFold{false, {}};
    }
    return AsciiFold{true, utf8};
}

double estimateHelveticaWidth(const std::string& asciiText, double fontSize) {
    // Helvetica 的字寬分佈：數字與多數小寫約 0.556 em、大寫約 0.667 em、
    // 標點與 i/l/. 約 0.28 em。分三段估算比一律 0.5 em 準得多，
    // 而準確度只影響對齊，不影響檔案是否合法。
    double units = 0.0;
    // 逐碼點而不是逐位元組：一個中文字是三個位元組，逐位元組會把它算成
    // 三個拉丁字的寬度，欄位的置中與靠右對齊因此整個偏掉。
    for (const char32_t codepoint : fonts::decodeUtf8(asciiText)) {
        if (codepoint >= 128) {
            // 非 ASCII 走內嵌字型的實際字寬。量測與內嵌必須是同一份字型，
            // 否則對齊會偏——而偏掉的欄位看起來只是「排版有點醜」，
            // 不會讓人聯想到字型。
            const std::uint16_t advance = fonts::CjkFontLibrary::instance().advanceFor(codepoint);
            units += advance != 0 ? advance : 1000.0;
            continue;
        }
        const auto c = static_cast<unsigned char>(codepoint);
        if (c == ' ') {
            units += 278.0;
        } else if (c == 'i' || c == 'l' || c == 'j' || c == '.' || c == ',' || c == '\'' ||
                   c == ':' || c == ';' || c == '|' || c == '!') {
            units += 250.0;
        } else if (c >= 'A' && c <= 'Z') {
            units += 667.0;
        } else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') {
            units += 833.0;
        } else {
            units += 556.0;
        }
    }
    return units / 1000.0 * fontSize;
}

FieldAppearance generateFieldAppearance(const FieldDefinition& definition,
                                        std::size_t radioIndex) {
    // 這一次外觀產生用到的中文字。彙整在一處而不是每個分支各自回報，
    // 因為一個欄位可能同時有值與說明文字，兩邊的字都要進同一份子集。
    std::set<char32_t> cjkUsed;
    if (const std::string problem = validate(definition); !problem.empty()) {
        return failure(problem);
    }

    domain::RectF sourceRect = definition.rectPt;
    if (definition.type == BuildFieldType::RadioGroup) {
        if (radioIndex >= definition.radios.size()) return failure("單選按鈕索引超出範圍");
        sourceRect = definition.radios[radioIndex].rectPt;
    }

    const domain::RectF rect = sourceRect.normalized();
    const double width = rect.width();
    const double height = rect.height();

    FieldAppearance out;
    out.bbox = domain::RectF{0.0, 0.0, width, height};

    const FieldAppearanceCharacteristics& mk = definition.appearance;

    switch (definition.type) {
        case BuildFieldType::Text:
        case BuildFieldType::Date:
        case BuildFieldType::ComboBox: {
            const AsciiFold folded = foldToWinAnsi(definition.value);
            if (!folded.ok) {
                return failure("欄位「" + definition.name +
                                "」的值含畫不出來的字元：不是可列印 ASCII，內嵌的 CJK 子集裡也沒有對應字形");
            }

            std::string content = "q\n";
            appendChrome(content, mk, width, height);

            const bool multiline =
                definition.multiline && definition.type == BuildFieldType::Text;
            const std::vector<std::string> lines =
                multiline ? splitLines(folded.text) : std::vector<std::string>{folded.text};
            const double fontSize =
                resolveFontSize(definition, height, static_cast<int>(lines.size()));

            // 文字必須被裁在欄位內。少了這段裁切，超長的值會畫到欄位外面，
            // 在密集的表單上會直接蓋住隔壁欄位的內容。
            content += "q\n";
            appendRect(content, kTextPadding, kTextPadding,
                       std::max(0.0, width - kTextPadding * 2.0 -
                                         (definition.type == BuildFieldType::ComboBox ? height * 0.6 : 0.0)),
                       std::max(0.0, height - kTextPadding * 2.0));
            content += "W\nn\n";

            if (!folded.text.empty()) {
                if (multiline) {
                    double y = height - kTextPadding - fontSize;
                    for (const std::string& line : lines) {
                        if (y < -fontSize) break;
                        appendTextRun(content, line, alignedX(definition, line, fontSize, width),
                                      y, fontSize, definition.textColor, &cjkUsed);
                        y -= fontSize * 1.2;
                    }
                } else {
                    // 單行垂直置中：Helvetica 的基線大約在 em 高度的 0.22 處，
                    // 直接用 (height - fontSize) / 2 會讓文字看起來偏上。
                    const double baseline = (height - fontSize) / 2.0 + fontSize * 0.22;
                    appendTextRun(content, folded.text,
                                  alignedX(definition, folded.text, fontSize, width), baseline,
                                  fontSize, definition.textColor, &cjkUsed);
                }
            }
            content += "Q\n";

            if (definition.type == BuildFieldType::ComboBox) {
                // 下拉三角。沒有它時下拉方塊與文字欄位長得一模一樣，
                // 使用者不會知道那裡可以展開。
                const double boxWidth = height * 0.6;
                const double cx = width - boxWidth / 2.0 - kTextPadding;
                const double cy = height / 2.0;
                const double half = std::min(4.0, boxWidth * 0.25);
                appendColor(content, FieldColor{0.25, 0.25, 0.25}, false);
                content += num(cx - half) + " " + num(cy + half * 0.6) + " m\n";
                content += num(cx + half) + " " + num(cy + half * 0.6) + " l\n";
                content += num(cx) + " " + num(cy - half * 0.7) + " l\nf\n";
            }

            content += "Q\n";
            out.states.push_back(FieldAppearanceState{{}, std::move(content), true});
            break;
        }

        case BuildFieldType::ListBox: {
            std::string content = "q\n";
            appendChrome(content, mk, width, height);

            const double fontSize =
                definition.fontSize > 0.0 ? definition.fontSize : kMaxAutoFontSize * 0.85;
            const double lineHeight = fontSize * 1.35;

            content += "q\n";
            appendRect(content, kTextPadding, kTextPadding,
                       std::max(0.0, width - kTextPadding * 2.0),
                       std::max(0.0, height - kTextPadding * 2.0));
            content += "W\nn\n";

            double y = height - kTextPadding - lineHeight;
            for (const std::string& option : definition.options) {
                if (y < -lineHeight) break;
                const AsciiFold folded = foldToWinAnsi(option);
                if (!folded.ok) {
                    return failure("欄位「" + definition.name +
                                    "」的選項含畫不出來的字元：不是可列印 ASCII，內嵌的 CJK 子集裡也沒有對應字形");
                }
                if (option == definition.value) {
                    // 選取項的反白。Acrobat 用的是系統選取色，這裡固定成
                    // 淡藍；顏色不一致不影響功能，缺了反白則看不出選了什麼。
                    appendColor(content, FieldColor{0.6, 0.75, 0.95}, false);
                    appendRect(content, kTextPadding, y - fontSize * 0.25,
                               width - kTextPadding * 2.0, lineHeight);
                    content += "f\n";
                }
                appendTextRun(content, folded.text, kTextPadding * 2.0, y, fontSize,
                              definition.textColor, &cjkUsed);
                y -= lineHeight;
            }
            content += "Q\nQ\n";
            out.states.push_back(FieldAppearanceState{{}, std::move(content), true});
            break;
        }

        case BuildFieldType::CheckBox: {
            std::string off = "q\n";
            appendChrome(off, mk, width, height);
            off += "Q\n";

            std::string on = "q\n";
            appendChrome(on, mk, width, height);
            on += checkMarkPath(width, height, definition.textColor);
            on += "Q\n";

            // /Off 必須存在。缺了它，未勾選的核取方塊在部分檢視器上
            // 會沿用上一次畫的外觀，看起來像是永遠打勾。
            out.states.push_back(FieldAppearanceState{"Off", std::move(off), false});
            out.states.push_back(
                FieldAppearanceState{definition.exportValue, std::move(on), false});
            break;
        }

        case BuildFieldType::RadioGroup: {
            const double radius = std::min(width, height) / 2.0;
            const double cx = width / 2.0;
            const double cy = height / 2.0;

            std::string off = "q\n";
            if (mk.backgroundColor.has_value()) {
                appendColor(off, *mk.backgroundColor, false);
                appendCircle(off, cx, cy, radius - mk.borderWidth / 2.0);
                off += "f\n";
            }
            if (mk.borderColor.has_value() && mk.borderWidth > 0.0) {
                appendColor(off, *mk.borderColor, true);
                off += num(mk.borderWidth) + " w\n";
                appendCircle(off, cx, cy, radius - mk.borderWidth / 2.0);
                off += "S\n";
            }
            off += "Q\n";

            std::string on = off;
            on.pop_back();  // 去掉尾端的 "Q\n"，把圓點畫在同一個圖形狀態裡
            on.pop_back();
            appendColor(on, definition.textColor, false);
            appendCircle(on, cx, cy, radius * 0.45);
            on += "f\nQ\n";

            out.states.push_back(FieldAppearanceState{"Off", std::move(off), false});
            out.states.push_back(FieldAppearanceState{
                definition.radios[radioIndex].exportValue, std::move(on), false});
            break;
        }

        case BuildFieldType::PushButton:
        case BuildFieldType::Image: {
            std::string content = "q\n";
            appendChrome(content, mk, width, height);

            const AsciiFold folded = foldToWinAnsi(mk.caption);
            if (!folded.ok) {
                return failure("欄位「" + definition.name +
                                "」的按鈕文字含畫不出來的字元：不是可列印 ASCII，內嵌的 CJK 子集裡也沒有對應字形");
            }
            const bool hasCaption = !folded.text.empty();
            if (hasCaption) {
                const double fontSize = resolveFontSize(definition, height, 1);
                const double textWidth = estimateHelveticaWidth(folded.text, fontSize);
                const double baseline = (height - fontSize) / 2.0 + fontSize * 0.22;
                appendTextRun(content, folded.text,
                              std::max(kTextPadding, (width - textWidth) / 2.0), baseline,
                              fontSize, definition.textColor, &cjkUsed);
            }
            content += "Q\n";
            out.states.push_back(FieldAppearanceState{{}, std::move(content), hasCaption});
            break;
        }

        case BuildFieldType::Barcode: {
            // validate() 已經確認 value 是非空的可列印 ASCII，這裡只需要
            // 再檢查一次編碼是否成功（例如矩形太窄放不下靜區）。條碼欄位
            // 沒有「畫不出來就留白」這個選項——留白的條碼會被誤判成
            // 「這裡本來就沒有內容」，比明確失敗更容易被忽略。
            const domain::barcode::Code128Result code = domain::barcode::encodeCode128(definition.value);
            if (!code.ok) return failure("欄位「" + definition.name + "」" + code.diagnostic);

            std::string content = "q\n";
            appendChrome(content, mk, width, height);

            // 靜區各佔可視寬度的一部分：模組數固定為 10（Code 128 建議下限），
            // 換算成點數需要先估出模組寬度，作法與
            // engine/enhance/barcode_stamp.cpp 一致。
            constexpr int kQuietZoneModules = 10;
            const double totalModules = static_cast<double>(code.totalModules);
            const double approxModuleWidth = width / (totalModules + kQuietZoneModules * 2.0);
            const double quietWidth = approxModuleWidth * static_cast<double>(kQuietZoneModules);
            const domain::RectF inkRect{quietWidth, kTextPadding, width - quietWidth, height};
            // 刻意不用 inkRect.normalized().isEmpty()：normalized() 會把
            // 上下顛倒的矩形直接交換回正常順序，顛倒的原因（底部固定留白
            // kTextPadding 超過矩形高度）反而被悄悄「修好」變成一個看似
            // 合法但物理上不合理的極薄矩形。這裡要偵測的正是「顛倒」本身，
            // 因此直接看未正規化的 width()/height()（right-left／top-bottom），
            // 負值就是矩形真的放不下。
            if (inkRect.width() <= 0.0 || inkRect.height() <= 0.0) {
                return failure("欄位「" + definition.name + "」的矩形太窄或太短，扣掉靜區與留白後沒有可畫墨的區域");
            }
            content += domain::barcode::barcodeBarsContentStream(code, inkRect);
            content += "Q\n";
            out.states.push_back(FieldAppearanceState{{}, std::move(content), false});
            break;
        }

        case BuildFieldType::Signature: {
            // 未簽章的簽章欄位只畫框。刻意不畫「請在此簽名」之類的提示文字：
            // 那段文字會被烙進外觀串流，簽署之後仍然留在頁面上。
            std::string content = "q\n";
            appendChrome(content, mk, width, height);
            content += "Q\n";
            out.states.push_back(FieldAppearanceState{{}, std::move(content), false});
            break;
        }
    }

    out.valid = true;
    // 這兩者必須成對送出去：宣告用了 CJK 卻沒給碼點，寫入層做不出子集，
    // 欄位的中文就整段消失而且沒有錯誤訊息。
    out.needsCjkFont = !cjkUsed.empty();
    out.cjkCodepoints = std::move(cjkUsed);
    return out;
}

}  // namespace alioth::engine::formbuild
