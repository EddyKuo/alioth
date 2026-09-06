#include "engine/signature/signature_appearance.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace alioth::engine::signature {
namespace {

// 內縮量。與註解家族的 kFreeTextPaddingPt 用同一個數字不是巧合——
// 兩者都是「文字與外框之間留多少」，用不同的值會讓簽章欄看起來與
// 其他標註格格不入。
constexpr double kPadding = 3.0;

// Helvetica 的概略字寬（1/1000 em）。與 engine/annotations/text_layout.cpp
// 的表同源；這裡重寫一份而不是連結過去，是為了讓 alioth_signature 維持
// 只依賴 domain 與 objects，不把註解子系統拖進簽章的相依圖。
double estimateWidth(const std::string& text, double fontSize) {
    double total = 0.0;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == ' ') {
            total += 278.0;
        } else if (u >= '0' && u <= '9') {
            total += 556.0;
        } else if (u == 'i' || u == 'l' || u == 'j' || u == '.' || u == ',' || u == ':') {
            total += 222.0;
        } else if (u >= 'A' && u <= 'Z') {
            total += 667.0;
        } else {
            total += 500.0;
        }
    }
    return total * fontSize / 1000.0;
}

std::string formatNumber(double value) {
    // 不用 printf 系列：LC_NUMERIC 被改成使用逗號小數點的地區時，
    // 產出的內容串流會整份損毀，而那種錯誤只在特定使用者機器上重現。
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(4);
    stream << std::fixed << value;
    std::string text = stream.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text.empty() || text == "-0") text = "0";
    return text;
}

bool isPrintableAscii(const std::string& text) {
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) return false;
    }
    return true;
}

// PDF 常值字串的跳脫。只跳脫必要的三個字元，不加外層括號——加括號是
// 序列化那一步的事，兩邊都加會讓括號配對失衡，而症狀是其後所有物件
// 解析錯位，不是單一字串顯示怪異。
std::string escapeLiteral(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (const char c : text) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}  // namespace

bool AppearanceImage::isValid() const {
    if (width <= 0 || height <= 0) return false;
    if (channels != 3 && channels != 4) return false;
    return pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                static_cast<std::size_t>(channels);
}

AppearanceResult buildSignatureAppearance(const SignatureAppearanceOptions& options) {
    AppearanceResult result;
    const auto fail = [&result](std::string message) {
        result.ok = false;
        result.diagnostic = std::move(message);
        return result;
    };

    const domain::RectF rect = options.rectPt.normalized();
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
        return fail("簽章外觀的矩形是空的");
    }

    // 外觀的座標系原點在 /BBox 的左下角，不是頁面座標——/Rect 的位置由
    // 註解字典決定，外觀只描述「這個框裡長什麼樣」。混淆這兩者會讓外觀
    // 被畫到頁面的另一個角落。
    const domain::RectF box{0.0, 0.0, rect.width(), rect.height()};
    result.bbox = box;

    std::vector<std::string> lines;
    if (!options.signerName.empty()) lines.push_back(options.signerName);
    if (!options.signingTime.empty()) lines.push_back(options.signingTime);
    if (!options.reason.empty()) lines.push_back("Reason: " + options.reason);
    if (!options.location.empty()) lines.push_back("Location: " + options.location);

    for (const std::string& line : lines) {
        if (!isPrintableAscii(line)) {
            // 靜默丟字會讓簽章欄顯示一個殘缺的姓名，而使用者不會發現。
            // CJK 的路徑是改用手寫簽名影像，見標頭說明。
            return fail("簽章外觀目前只支援可列印 ASCII；非 ASCII 內容請改用手寫簽名影像");
        }
    }

    const bool hasImage = options.image.isValid();
    if (!hasImage && !options.image.pixels.empty()) {
        // 有給像素但尺寸對不上：畫出來會是斜的或直接讀到界外。
        return fail("簽章外觀的影像尺寸與像素資料不符");
    }
    if (!hasImage && lines.empty()) {
        return fail("簽章外觀既沒有影像也沒有文字");
    }

    std::string content = "q\n";

    if (options.drawBorder) {
        const double inset = 0.5;
        content += "0.4 0.4 0.4 RG\n1 w\n";
        content += formatNumber(box.left + inset) + " " + formatNumber(box.bottom + inset) + " " +
                   formatNumber(box.width() - inset * 2.0) + " " +
                   formatNumber(box.height() - inset * 2.0) + " re\nS\n";
    }

    double textLeft = box.left + kPadding;
    if (hasImage) {
        // 影像佔左側，最多一半寬度；等比例縮放後垂直置中。拉伸簽名會讓
        // 筆跡變形，而那正是使用者用來辨認「這是我的簽名」的東西。
        const double maxWidth = box.width() * 0.5 - kPadding * 2.0;
        const double maxHeight = box.height() - kPadding * 2.0;
        if (maxWidth > 0.0 && maxHeight > 0.0) {
            const double aspect = static_cast<double>(options.image.width) /
                                  static_cast<double>(options.image.height);
            double drawWidth = maxWidth;
            double drawHeight = drawWidth / aspect;
            if (drawHeight > maxHeight) {
                drawHeight = maxHeight;
                drawWidth = drawHeight * aspect;
            }
            const double x = box.left + kPadding;
            const double y = box.bottom + (box.height() - drawHeight) / 2.0;
            content += "q\n" + formatNumber(drawWidth) + " 0 0 " + formatNumber(drawHeight) + " " +
                       formatNumber(x) + " " + formatNumber(y) + " cm\n/Im0 Do\nQ\n";
            result.needsImage = true;
            textLeft = x + drawWidth + kPadding;
        }
    }

    if (!lines.empty()) {
        const double available = box.right - textLeft - kPadding;
        if (available <= 0.0) {
            return fail("簽章外觀的矩形太窄，影像佔滿之後沒有空間放文字");
        }

        // 字級由「最長那一行要塞得進去」與「所有行要疊得下」共同決定。
        // 只看其中一個的話，不是文字滿出右邊就是滿出上方。
        double fontSize = std::min(box.height() / (static_cast<double>(lines.size()) * 1.3),
                                   12.0);
        for (const std::string& line : lines) {
            const double natural = estimateWidth(line, fontSize);
            if (natural > available && natural > 0.0) {
                fontSize = std::min(fontSize, fontSize * available / natural);
            }
        }
        if (fontSize < 1.0) {
            return fail("簽章外觀的矩形太小，文字縮到看不見");
        }

        const double lineHeight = fontSize * 1.3;
        const double blockHeight = lineHeight * static_cast<double>(lines.size());
        double baseline = box.bottom + (box.height() + blockHeight) / 2.0 - lineHeight * 0.85;

        content += "0 0 0 rg\nBT\n/Helv " + formatNumber(fontSize) + " Tf\n";
        double previousX = 0.0;
        double previousY = 0.0;
        bool first = true;
        for (const std::string& line : lines) {
            // Td 是相對位移，不是絕對座標。第一次從原點算起，之後每次
            // 只移動差值——寫成絕對座標會讓每一行都疊在越來越遠的地方。
            const double dx = first ? textLeft : textLeft - previousX;
            const double dy = first ? baseline : baseline - previousY;
            content += formatNumber(dx) + " " + formatNumber(dy) + " Td\n";
            content += "(" + escapeLiteral(line) + ") Tj\n";
            previousX = textLeft;
            previousY = baseline;
            baseline -= lineHeight;
            first = false;
        }
        content += "ET\n";
        result.needsFont = true;
    }

    content += "Q\n";
    result.content = std::move(content);
    result.ok = true;
    return result;
}

}  // namespace alioth::engine::signature
