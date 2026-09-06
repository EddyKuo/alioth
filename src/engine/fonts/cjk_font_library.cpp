#include "engine/fonts/cjk_font_library.h"

#include <QFile>

#include "platform/font_paths.h"

namespace alioth::engine::fonts {

CjkFontLibrary& CjkFontLibrary::instance() {
    // 函式區域靜態：初始化是執行緒安全的，而且不依賴翻譯單元的初始化順序。
    static CjkFontLibrary library;
    return library;
}

void CjkFontLibrary::ensureLoaded() {
    if (loaded_) return;
    loaded_ = true;

    const platform::CjkFontSource source = platform::findCjkFont();
    if (!source.isValid()) {
        diagnostic_ = "找不到可用的 CJK 字型。已尋找：";
        for (const QString& candidate : platform::cjkFontCandidates()) {
            diagnostic_ += "\n  " + candidate.toStdString();
        }
        return;
    }

    QFile file(source.path);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostic_ = "無法讀取字型檔：" + source.path.toStdString();
        return;
    }
    const QByteArray raw = file.readAll();
    file.close();
    bytes_.assign(raw.constData(), static_cast<std::size_t>(raw.size()));
    baseName_ = source.baseName.toStdString();

    // 先做一次空子集，把字型度量（unitsPerEm、bbox、ascent/descent）與 cmap
    // 讀出來備用。這一步同時驗證字型格式——TrueType Collection 與 CFF 會在
    // 這裡就被擋下，而不是等到使用者真的打了中文才失敗。
    metrics_ = subsetTrueType(bytes_, {});
    if (!metrics_.ok) {
        diagnostic_ = "字型格式不支援（" + source.path.toStdString() + "）：" + metrics_.diagnostic;
        bytes_.clear();
    }
}

bool CjkFontLibrary::available() {
    ensureLoaded();
    return !bytes_.empty();
}

std::string CjkFontLibrary::diagnostic() {
    ensureLoaded();
    return diagnostic_;
}

std::string CjkFontLibrary::baseName() {
    ensureLoaded();
    return baseName_;
}

std::uint16_t CjkFontLibrary::advanceFor(char32_t codepoint) {
    if (!available()) return 0;
    if (const auto it = advanceCache_.find(codepoint); it != advanceCache_.end()) {
        return it->second;
    }

    // 單字子集只是為了問寬度。這比自己再解析一次 cmap 便宜得多，而且保證與
    // 內嵌時用的是同一條程式碼路徑——兩條路徑算出不同答案正是要避免的事。
    const SubsetResult single = subsetTrueType(bytes_, {codepoint});
    std::uint16_t advance = 0;
    if (single.ok) {
        if (const auto glyph = single.glyphForCodepoint.find(codepoint);
            glyph != single.glyphForCodepoint.end()) {
            if (const auto width = single.advanceForGlyph.find(glyph->second);
                width != single.advanceForGlyph.end()) {
                // 換算成千分之一 em，與 Helvetica 的字寬表同單位。
                const std::uint16_t unitsPerEm = single.unitsPerEm != 0 ? single.unitsPerEm : 1000;
                advance = static_cast<std::uint16_t>(
                    (static_cast<double>(width->second) * 1000.0 / unitsPerEm) + 0.5);
            }
        }
    }
    advanceCache_.emplace(codepoint, advance);
    return advance;
}

std::uint16_t CjkFontLibrary::glyphFor(char32_t codepoint) {
    if (!available()) return 0;
    if (const auto it = glyphCache_.find(codepoint); it != glyphCache_.end()) return it->second;

    const SubsetResult single = subsetTrueType(bytes_, {codepoint});
    std::uint16_t glyph = 0;
    if (single.ok) {
        if (const auto it = single.glyphForCodepoint.find(codepoint);
            it != single.glyphForCodepoint.end()) {
            glyph = it->second;
        }
    }
    glyphCache_.emplace(codepoint, glyph);
    return glyph;
}

SubsetResult CjkFontLibrary::subsetFor(const std::set<char32_t>& codepoints) {
    if (!available()) {
        SubsetResult result;
        result.diagnostic = diagnostic_;
        return result;
    }
    return subsetTrueType(bytes_, codepoints);
}

void CjkFontLibrary::overrideFontBytesForTesting(std::string bytes, std::string baseName) {
    loaded_ = true;
    bytes_ = std::move(bytes);
    baseName_ = std::move(baseName);
    diagnostic_.clear();
    advanceCache_.clear();
    glyphCache_.clear();
    metrics_ = subsetTrueType(bytes_, {});
    if (!metrics_.ok) bytes_.clear();
}

}  // namespace alioth::engine::fonts
