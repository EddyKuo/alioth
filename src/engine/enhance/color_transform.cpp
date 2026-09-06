#include "engine/enhance/color_transform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

#include "domain/annotation.h"
#include "engine/enhance/image_codec.h"
#include "engine/objects/pdf_parser.h"

namespace alioth::engine::enhance {

namespace {

using domain::ColorRgb;
using domain::enhance::ColorTransformSettings;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::PdfStream;

// ---------------------------------------------------------------------------
// 內容串流語彙分析（僅供本檔使用）。
//
// 刻意不重用 engine/redaction/content_redactor.cpp 的 ContentLexer：那個類別
// 是該檔案的實作細節（匿名命名空間，未對外公開），而重新公開它會讓兩個本來
// 無關的工作包（塗黑、色彩轉換）背上共同相依。內容串流的語彙規則是 PDF
// 規格的一部分而非兩邊自創，各自實作一份小的、只做到「找出 token 邊界」
// 程度的版本，重複的成本遠低於耦合的成本。
// ---------------------------------------------------------------------------

enum class TokenKind { Number, Operator, Other, End };

struct Token {
    TokenKind kind{TokenKind::End};
    std::size_t begin{0};
    std::size_t end{0};
    double number{0.0};
    std::string text;
};

[[nodiscard]] bool isWhitespaceByte(unsigned char c) noexcept {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

[[nodiscard]] bool isDelimiterByte(unsigned char c) noexcept {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '/' || c == '%';
}

class MiniLexer {
public:
    explicit MiniLexer(const std::string& bytes) : bytes_(bytes) {}

    [[nodiscard]] Token next() {
        skipTrivia();
        Token token;
        token.begin = pos_;
        if (pos_ >= bytes_.size()) {
            token.end = pos_;
            return token;
        }
        const char c = bytes_[pos_];
        if (c == '(') return literalString(token);
        if (c == '<') {
            if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '<') return balanced(token, "<<", ">>");
            return hexString(token);
        }
        if (c == '[') return balancedArray(token);
        if (c == '/') return name(token);
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') return number(token);
        return oper(token);
    }

private:
    void skipTrivia() {
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c)) { ++pos_; continue; }
            if (c == '%') {
                while (pos_ < bytes_.size() && bytes_[pos_] != '\n' && bytes_[pos_] != '\r') ++pos_;
                continue;
            }
            return;
        }
    }

    Token literalString(Token token) {
        token.kind = TokenKind::Other;
        ++pos_;
        int depth = 1;
        while (pos_ < bytes_.size() && depth > 0) {
            const char c = bytes_[pos_++];
            if (c == '\\') { if (pos_ < bytes_.size()) ++pos_; continue; }
            if (c == '(') { ++depth; continue; }
            if (c == ')') { --depth; continue; }
        }
        token.end = pos_;
        return token;
    }

    Token hexString(Token token) {
        token.kind = TokenKind::Other;
        ++pos_;
        while (pos_ < bytes_.size() && bytes_[pos_] != '>') ++pos_;
        if (pos_ < bytes_.size()) ++pos_;
        token.end = pos_;
        return token;
    }

    // 陣列可能巢狀（例如 TJ 的位移陣列裡混著字串），用深度計數處理。
    Token balancedArray(Token token) {
        token.kind = TokenKind::Other;
        int depth = 0;
        while (pos_ < bytes_.size()) {
            const char c = bytes_[pos_];
            if (c == '(') { literalStringSkipOnly(); continue; }
            if (c == '[') { ++depth; ++pos_; continue; }
            if (c == ']') { --depth; ++pos_; if (depth == 0) break; continue; }
            ++pos_;
        }
        token.end = pos_;
        return token;
    }

    void literalStringSkipOnly() {
        ++pos_;
        int depth = 1;
        while (pos_ < bytes_.size() && depth > 0) {
            const char c = bytes_[pos_++];
            if (c == '\\') { if (pos_ < bytes_.size()) ++pos_; continue; }
            if (c == '(') { ++depth; continue; }
            if (c == ')') { --depth; continue; }
        }
    }

    Token balanced(Token token, const char* openTok, const char* closeTok) {
        token.kind = TokenKind::Other;
        (void)openTok;
        pos_ += 2;
        int depth = 1;
        while (pos_ < bytes_.size() && depth > 0) {
            if (pos_ + 1 < bytes_.size() && bytes_[pos_] == '<' && bytes_[pos_ + 1] == '<') {
                ++depth; pos_ += 2; continue;
            }
            if (pos_ + 1 < bytes_.size() && bytes_[pos_] == '>' && bytes_[pos_ + 1] == '>') {
                --depth; pos_ += 2; continue;
            }
            ++pos_;
        }
        (void)closeTok;
        token.end = pos_;
        return token;
    }

    Token name(Token token) {
        token.kind = TokenKind::Other;
        ++pos_;
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c) || isDelimiterByte(c)) break;
            ++pos_;
        }
        token.end = pos_;
        return token;
    }

    Token number(Token token) {
        token.kind = TokenKind::Number;
        const std::size_t start = pos_;
        while (pos_ < bytes_.size()) {
            const char c = bytes_[pos_];
            if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') { ++pos_; continue; }
            break;
        }
        token.text = bytes_.substr(start, pos_ - start);
        token.number = std::strtod(token.text.c_str(), nullptr);
        token.end = pos_;
        return token;
    }

    Token oper(Token token) {
        token.kind = TokenKind::Operator;
        const std::size_t start = pos_;
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c) || isDelimiterByte(c)) break;
            ++pos_;
        }
        if (pos_ == start) ++pos_;
        token.text = bytes_.substr(start, pos_ - start);
        token.end = pos_;
        return token;
    }

    const std::string& bytes_;
    std::size_t pos_{0};
};

[[nodiscard]] ColorRgb rgbFromOperands(const std::string& op, const std::vector<double>& n) {
    if (op == "g" || op == "G" || (n.size() == 1)) return ColorRgb{n[0], n[0], n[0]};
    if (op == "rg" || op == "RG" || n.size() == 3) return ColorRgb{n[0], n[1], n[2]};
    // k / K，或 4 個運算元的 sc/scn：以 DeviceCMYK 的樸素換算轉成 RGB
    // （見 domain/enhance.h 的覆蓋範圍說明，這是啟發式而非色彩管理）。
    const double c = n[0], m = n[1], y = n[2], k = n[3];
    return ColorRgb{(1.0 - c) * (1.0 - k), (1.0 - m) * (1.0 - k), (1.0 - y) * (1.0 - k)};
}

[[nodiscard]] std::string formatNum(double v) {
    // 三位小數足夠表示色彩分量（0–1），且不會像更高精度那樣讓內容串流
    // 徒增體積。
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", v);
    std::string s(buffer);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

}  // namespace

ContentColorTransformResult transformContentStreamColors(const std::string& content,
                                                          const domain::enhance::ColorTransformSettings& settings) {
    ContentColorTransformResult result;
    MiniLexer lexer(content);

    struct Replacement {
        std::size_t begin;
        std::size_t end;
        std::string text;
    };
    std::vector<Replacement> replacements;

    std::vector<double> numbers;
    std::size_t numbersBegin = 0;

    static const std::set<std::string> kTargetOps = {"g", "G", "rg", "RG", "k", "K",
                                                      "sc", "SC", "scn", "SCN"};

    while (true) {
        const Token token = lexer.next();
        if (token.kind == TokenKind::End) break;

        if (token.kind == TokenKind::Number) {
            if (numbers.empty()) numbersBegin = token.begin;
            numbers.push_back(token.number);
            continue;
        }

        if (token.kind == TokenKind::Operator && kTargetOps.count(token.text) != 0) {
            const bool stroke = !token.text.empty() &&
                                token.text.front() >= 'A' && token.text.front() <= 'Z';
            const bool arityOk = numbers.size() == 1 || numbers.size() == 3 || numbers.size() == 4;
            if (arityOk) {
                const ColorRgb original = rgbFromOperands(token.text, numbers);
                const ColorRgb transformed = domain::enhance::applyColorTransform(original, settings);

                std::string replacementText;
                if (settings.mode == domain::enhance::ColorTransformMode::Grayscale) {
                    const double gray = domain::enhance::luminance601(transformed);
                    replacementText = formatNum(gray) + " " + (stroke ? "G" : "g");
                } else {
                    replacementText = formatNum(transformed.r) + " " + formatNum(transformed.g) +
                                      " " + formatNum(transformed.b) + " " + (stroke ? "RG" : "rg");
                }
                replacements.push_back(Replacement{numbersBegin, token.end, replacementText});
                ++result.rewritten;
            } else {
                // 運算元數量不是 1/3/4：多半是 scn/SCN 帶著 Pattern 名稱或
                // 非裝置色彩空間的運算元組合，明確計入未覆蓋而不是靜默跳過。
                ++result.skippedUnsupported;
            }
            numbers.clear();
            continue;
        }

        // 任何其他 token（包含不在目標清單裡的運算子）都會中斷「連續數字」
        // 的假設，因此清空緩衝——但如果緩衝裡曾經有數字卻沒有配對到目標
        // 運算子，那些數字本來就不該被當成色彩運算元，不需要額外處理。
        numbers.clear();
    }

    if (replacements.empty()) {
        result.content = content;
        return result;
    }

    std::string out;
    out.reserve(content.size());
    std::size_t cursor = 0;
    for (const Replacement& r : replacements) {
        out.append(content, cursor, r.begin - cursor);
        out += r.text;
        cursor = r.end;
    }
    out.append(content, cursor, content.size() - cursor);
    result.content = std::move(out);
    return result;
}

namespace {

struct ImageInfo {
    std::string filter;
    std::string colorSpace;
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t bitsPerComponent{8};
};

[[nodiscard]] std::string singleFilterName(const PdfSourceDocument& source, const PdfObject& value) {
    const PdfObject resolved = source.resolve(value);
    if (resolved.isName()) return resolved.asName();
    if (const PdfArray* array = resolved.asArray(); array != nullptr && array->size() == 1) {
        return source.resolve((*array)[0]).asName();
    }
    return {};
}

[[nodiscard]] bool readImageInfo(const PdfSourceDocument& source, const PdfDictionary& dict,
                                 ImageInfo& out) {
    const PdfObject* subtype = dict.find("Subtype");
    if (subtype == nullptr || !source.resolve(*subtype).isName("Image")) return false;
    const PdfObject* width = dict.find("Width");
    const PdfObject* height = dict.find("Height");
    if (width == nullptr || height == nullptr) return false;
    out.width = static_cast<std::int32_t>(source.resolve(*width).asInteger());
    out.height = static_cast<std::int32_t>(source.resolve(*height).asInteger());
    if (const PdfObject* bits = dict.find("BitsPerComponent"); bits != nullptr) {
        out.bitsPerComponent = static_cast<std::int32_t>(source.resolve(*bits).asInteger());
    }
    if (const PdfObject* filter = dict.find("Filter"); filter != nullptr) {
        out.filter = singleFilterName(source, *filter);
    }
    if (const PdfObject* space = dict.find("ColorSpace"); space != nullptr) {
        out.colorSpace = source.resolve(*space).asName();
    }
    return out.width > 0 && out.height > 0;
}

[[nodiscard]] DecodedImage decodeImageObject(const PdfSourceDocument& source, const PdfStream& stream,
                                             const ImageInfo& info) {
    if (info.filter == "DCTDecode") return decodeJpeg(stream.data);
    if (info.filter.empty() || info.filter == "FlateDecode") {
        const objects::DecodeResult decoded = objects::decodeStream(
            stream, [&source](const PdfRef& ref) { return source.object(ref.number); });
        if (!decoded.ok) {
            DecodedImage failed;
            failed.diagnostic = decoded.diagnostic;
            return failed;
        }
        return decodeRawSamples(decoded.data, info.width, info.height, info.bitsPerComponent,
                                info.colorSpace);
    }
    DecodedImage unsupported;
    unsupported.diagnostic = "不支援的濾鏡：" + info.filter;
    return unsupported;
}

}  // namespace

ColorTransformResult convertColors(objects::IncrementalAppender& appender,
                                   const ColorTransformSettings& settings) {
    ColorTransformResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟";
        return result;
    }
    if (!settings.valid()) {
        result.diagnostic = "色彩轉換設定不合法";
        return result;
    }

    const PdfSourceDocument& source = appender.source();
    const auto pageCount = static_cast<std::int32_t>(source.pages().size());

    if (settings.transformVectorGraphics) {
        for (std::int32_t index = 0; index < pageCount; ++index) {
            if (!settings.appliesToPage(index)) continue;
            const PdfRef page = source.pages()[static_cast<std::size_t>(index)];

            // /Contents 可能是單一串流物件，也可能是串流陣列；兩種都要處理，
            // 否則多內容串流的頁面只會轉換其中一段，出現「半灰半彩」——
            // 正是本功能最不該犯的錯誤。
            const PdfObject* contentsEntry = nullptr;
            // 同一個坑：resolve() 回傳暫時物件，必須先綁進具名變數
            // pageObject 才能安全取指標，否則 pageDict／contentsEntry
            // 兩層指標全部懸空。
            const PdfObject pageObject = source.resolve(PdfObject{page});
            const PdfDictionary* pageDict = pageObject.asDictionary();
            if (pageDict != nullptr) contentsEntry = pageDict->find("Contents");
            if (contentsEntry == nullptr) continue;

            // 注意：判斷「單一串流」要看 contentsEntry 本身（尚未 resolve）
            // 是不是參照，不能看 resolvedContents——resolve() 已經把參照解成
            // 目標物件（通常是 PdfStream），解完之後 isRef() 恆為 false，
            // 用它判斷會讓單一內容串流的頁面永遠被判定成「兩種都不是」，
            // streamObjectNumbers 保持空陣列，整頁的向量圖形因此完全沒被
            // 轉換卻不會報錯——這正是本功能最不能犯的「靜默漏轉換」。
            std::vector<int> streamObjectNumbers;
            const PdfObject resolvedContents = source.resolve(*contentsEntry);
            if (contentsEntry->isRef()) {
                streamObjectNumbers.push_back(contentsEntry->asRef().number);
            } else if (const PdfArray* array = resolvedContents.asArray(); array != nullptr) {
                for (const PdfObject& entry : *array) {
                    if (entry.isRef()) streamObjectNumbers.push_back(entry.asRef().number);
                }
            }

            for (const int objectNumber : streamObjectNumbers) {
                const PdfObject streamObject = source.object(objectNumber);
                const PdfStream* stream = streamObject.asStream();
                if (stream == nullptr) continue;
                const objects::DecodeResult decoded = objects::decodeStream(
                    *stream, [&source](const PdfRef& ref) { return source.object(ref.number); });
                if (!decoded.ok) continue;  // 解不開的串流原樣保留，不假裝轉換成功

                const ContentColorTransformResult transformed =
                    transformContentStreamColors(decoded.data, settings);
                result.report.contentOperatorsRewritten += transformed.rewritten;
                result.report.contentOperatorsSkippedUnsupported += transformed.skippedUnsupported;
                if (transformed.rewritten == 0) continue;

                PdfStream newStream = *stream;
                newStream.dict.remove("Filter");
                newStream.dict.remove("DecodeParms");
                newStream.data = transformed.content;
                if (!appender.updateObject(objectNumber, PdfObject{std::move(newStream)})) {
                    ++result.report.contentStreamsFailed;
                }
            }
        }
    }

    if (settings.transformImages) {
        std::set<int> visited;
        for (std::int32_t index = 0; index < pageCount; ++index) {
            if (!settings.appliesToPage(index)) continue;
            const PdfRef page = source.pages()[static_cast<std::size_t>(index)];
            const PdfObject resources =
                source.resolve(source.inheritedPageAttribute(page, "Resources"));
            const PdfDictionary* resourceDict = resources.asDictionary();
            if (resourceDict == nullptr) continue;
            const PdfObject* xobjectEntry = resourceDict->find("XObject");
            if (xobjectEntry == nullptr) continue;
            // 一定要先綁進具名變數再取指標：resolve() 回傳的是暫時物件，
            // 直接對它的回傳值鏈式呼叫 .asDictionary() 會在同一敘述句結束時
            // 讓暫時物件解構，留下一個懸空指標——這個坑已經在這裡踩過一次
            // （症狀是 entries() 讀出來永遠是 0，不是崩潰，非常難查）。
            const PdfObject xobjects = source.resolve(*xobjectEntry);
            const PdfDictionary* xobjectDict = xobjects.asDictionary();
            if (xobjectDict == nullptr) continue;

            for (const auto& [name, value] : xobjectDict->entries()) {
                if (!value.isRef()) continue;
                const int objectNumber = value.asRef().number;
                if (!visited.insert(objectNumber).second) continue;

                const PdfObject imageObject = source.object(objectNumber);
                const PdfStream* stream = imageObject.asStream();
                if (stream == nullptr) continue;

                ImageInfo info;
                if (!readImageInfo(source, stream->dict, info)) continue;

                DecodedImage decoded = decodeImageObject(source, *stream, info);
                if (!decoded.ok) {
                    ++result.report.imagesSkippedUnsupported;
                    continue;
                }

                PixelBuffer pixels = std::move(decoded.pixels);
                for (std::int32_t y = 0; y < pixels.height(); ++y) {
                    std::uint8_t* row = pixels.scanline(y);
                    for (std::int32_t x = 0; x < pixels.width(); ++x) {
                        std::uint8_t* px = row + x * 4;
                        const ColorRgb input{static_cast<double>(px[2]) / 255.0,
                                            static_cast<double>(px[1]) / 255.0,
                                            static_cast<double>(px[0]) / 255.0};
                        const ColorRgb out = domain::enhance::applyColorTransform(input, settings);
                        px[2] = static_cast<std::uint8_t>(std::clamp(out.r, 0.0, 1.0) * 255.0 + 0.5);
                        px[1] = static_cast<std::uint8_t>(std::clamp(out.g, 0.0, 1.0) * 255.0 + 0.5);
                        px[0] = static_cast<std::uint8_t>(std::clamp(out.b, 0.0, 1.0) * 255.0 + 0.5);
                    }
                }

                domain::enhance::CompressionSettings compression;
                compression.codec = info.filter == "DCTDecode" ? domain::enhance::ImageCodec::Jpeg
                                                                : domain::enhance::ImageCodec::Flate;
                compression.jpegQuality = 90;
                const EncodedImage encoded = encodeImage(pixels, compression);
                if (!encoded.ok) {
                    ++result.report.imagesFailed;
                    continue;
                }

                // 就地覆寫：複製原字典，只改動與樣本資料本身相關的鍵，
                // /SMask 等其餘鍵原樣保留——色彩轉換不改變透明度。
                PdfStream newStream = *stream;
                newStream.dict.set("Filter", objects::makeName(encoded.filter));
                newStream.dict.set("ColorSpace", objects::makeName(encoded.colorSpace));
                newStream.dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});
                newStream.dict.remove("DecodeParms");
                newStream.dict.remove("Decode");
                newStream.data = encoded.data;

                if (appender.updateObject(objectNumber, PdfObject{std::move(newStream)})) {
                    ++result.report.imagesTransformed;
                } else {
                    ++result.report.imagesFailed;
                }
            }
        }
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::enhance
