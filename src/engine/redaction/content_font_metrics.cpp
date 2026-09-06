#include "engine/redaction/content_font_metrics.h"

#include <algorithm>

namespace alioth::engine::redaction {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;

// 字寬單位是 1/1000 em（Type3 除外，但 Type3 的 /FontMatrix 罕見到不值得
// 為它把整條路徑複雜化；它會落到預設寬度，方向是偏大，因此安全）。
constexpr double kGlyphUnits = 1000.0;

void collectSimpleWidths(const PdfDictionary& dict, const Resolver& resolve,
                         std::map<std::uint32_t, double>& out) {
    const PdfObject* firstChar = dict.find("FirstChar");
    const PdfObject* widths = dict.find("Widths");
    if (firstChar == nullptr || widths == nullptr) return;

    const PdfObject resolvedWidths = resolve(*widths);
    const PdfArray* array = resolvedWidths.asArray();
    if (array == nullptr) return;

    const std::int64_t base = resolve(*firstChar).asInteger(0);
    for (std::size_t i = 0; i < array->size(); ++i) {
        const PdfObject value = resolve((*array)[i]);
        if (!value.isNumber()) continue;
        out[static_cast<std::uint32_t>(base + static_cast<std::int64_t>(i))] =
            value.asNumber(0.0) / kGlyphUnits;
    }
}

// CID 字型的 /W 有兩種語法交錯出現：「c [w …]」與「cFirst cLast w」。
// 兩者靠下一個元素是不是陣列來區分，判斷錯誤會把整段寬度表位移一格。
void collectCidWidths(const PdfObject& widthArray, const Resolver& resolve,
                      std::map<std::uint32_t, double>& out) {
    const PdfArray* array = widthArray.asArray();
    if (array == nullptr) return;

    std::size_t i = 0;
    while (i < array->size()) {
        const PdfObject first = resolve((*array)[i]);
        if (!first.isNumber() || i + 1 >= array->size()) break;
        const auto start = static_cast<std::uint32_t>(first.asInteger(0));

        const PdfObject second = resolve((*array)[i + 1]);
        if (const PdfArray* run = second.asArray()) {
            for (std::size_t k = 0; k < run->size(); ++k) {
                const PdfObject width = resolve((*run)[k]);
                if (width.isNumber()) {
                    out[start + static_cast<std::uint32_t>(k)] = width.asNumber(0.0) / kGlyphUnits;
                }
            }
            i += 2;
            continue;
        }
        if (i + 2 >= array->size()) break;
        const auto last = static_cast<std::uint32_t>(second.asInteger(0));
        const double width = resolve((*array)[i + 2]).asNumber(0.0) / kGlyphUnits;
        // 上限保護：惡意檔案可以寫 0 到 2^31 的區間，逐一填表會把記憶體吃光。
        const std::uint32_t end = std::min<std::uint32_t>(last, start + 65535u);
        for (std::uint32_t code = start; code <= end && code >= start; ++code) {
            out[code] = width;
        }
        i += 3;
    }
}

}  // namespace

FontMetrics FontMetrics::fromFontDictionary(const PdfObject& fontDict, const Resolver& resolve) {
    FontMetrics metrics;
    const PdfObject font = resolve(fontDict);
    const PdfDictionary* dict = font.asDictionary();
    if (dict == nullptr) return metrics;

    const PdfObject* subtype = dict->find("Subtype");
    const bool isType0 = subtype != nullptr && subtype->isName("Type0");

    if (isType0) {
        const PdfObject* encoding = dict->find("Encoding");
        // 只有 Identity-H / Identity-V 能靠字碼直接查 /W。其他 CMap 需要完整的
        // 編碼表，這裡退回「兩位元組 + 預設寬度」，方向仍是偏大。
        metrics.twoByte_ = true;
        if (encoding != nullptr && encoding->isName()) {
            const std::string name = encoding->asName();
            metrics.twoByte_ = name.rfind("Identity", 0) == 0 || name.find("UCS2") != std::string::npos ||
                               name.find("UniGB") != std::string::npos ||
                               name.find("UniCNS") != std::string::npos ||
                               name.find("UniJIS") != std::string::npos ||
                               name.find("UniKS") != std::string::npos;
        }

        PdfObject descendants;
        if (const PdfObject* value = dict->find("DescendantFonts")) descendants = resolve(*value);
        const PdfArray* array = descendants.asArray();
        if (array != nullptr && !array->empty()) {
            const PdfObject descendant = resolve((*array)[0]);
            if (const PdfDictionary* cid = descendant.asDictionary()) {
                if (const PdfObject* dw = cid->find("DW")) {
                    metrics.defaultWidth_ = resolve(*dw).asNumber(1000.0) / kGlyphUnits;
                } else {
                    metrics.defaultWidth_ = 1.0;  // /DW 預設值就是 1000
                }
                if (const PdfObject* w = cid->find("W")) {
                    collectCidWidths(resolve(*w), resolve, metrics.widths_);
                }
                if (const PdfObject* descriptor = cid->find("FontDescriptor")) {
                    const PdfObject resolved = resolve(*descriptor);
                    if (const PdfDictionary* fd = resolved.asDictionary()) {
                        if (const PdfObject* ascent = fd->find("Ascent")) {
                            metrics.ascent_ = resolve(*ascent).asNumber(1000.0) / kGlyphUnits;
                        }
                        if (const PdfObject* descent = fd->find("Descent")) {
                            metrics.descent_ = resolve(*descent).asNumber(-350.0) / kGlyphUnits;
                        }
                    }
                }
            }
        }
        return metrics;
    }

    collectSimpleWidths(*dict, resolve, metrics.widths_);
    if (const PdfObject* descriptor = dict->find("FontDescriptor")) {
        const PdfObject resolved = resolve(*descriptor);
        if (const PdfDictionary* fd = resolved.asDictionary()) {
            if (const PdfObject* missing = fd->find("MissingWidth")) {
                metrics.defaultWidth_ = resolve(*missing).asNumber(1000.0) / kGlyphUnits;
            }
            if (const PdfObject* ascent = fd->find("Ascent")) {
                metrics.ascent_ = resolve(*ascent).asNumber(1000.0) / kGlyphUnits;
            }
            if (const PdfObject* descent = fd->find("Descent")) {
                metrics.descent_ = resolve(*descent).asNumber(-350.0) / kGlyphUnits;
            }
        }
    }
    return metrics;
}

std::vector<std::uint32_t> FontMetrics::decode(const std::string& bytes) const {
    std::vector<std::uint32_t> codes;
    if (twoByte_) {
        codes.reserve(bytes.size() / 2 + 1);
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
            codes.push_back((static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 8) |
                            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])));
        }
        // 奇數長度的字串不合法，但真實檔案裡存在。補一個字碼讓寬度不會少算。
        if (bytes.size() % 2 == 1) {
            codes.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.back())));
        }
        return codes;
    }
    codes.reserve(bytes.size());
    for (const char c : bytes) {
        codes.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
    }
    return codes;
}

double FontMetrics::width(std::uint32_t code) const {
    const auto it = widths_.find(code);
    return it == widths_.end() ? defaultWidth_ : it->second;
}

}  // namespace alioth::engine::redaction
