#include "engine/layers/ocg_flatten.h"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "engine/objects/pdf_parser.h"
#include "engine/redaction/pdf_document_rewriter.h"

namespace alioth::engine::layers {

namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfStream;
using redaction::PdfDocumentRewriter;

// Form XObject 巢狀深度上限。PDF 是不可信任輸入，惡意或損毀的檔案可以讓
// Form 互相呼叫到任意深，這裡與 content_redactor 採同一個上限值。
constexpr int kMaxFormDepth = 12;

bool isWhitespaceByte(unsigned char c) {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

bool isDelimiterByte(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '/' || c == '%';
}

enum class TokenKind {
    Number,
    String,
    Name,
    ArrayStart,
    ArrayEnd,
    DictStart,
    DictEnd,
    Operator,
    End,
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::size_t begin{0};
    std::size_t end{0};
    std::string text{};  // 只有 Name／Operator 需要文字內容
};

// 內容串流的最小語彙分析，只服務標記內容攤平：正確跳過字串／名稱／字典的
// 邊界與 BI…EI 二進位資料，不解讀圖形狀態或座標——可見性判斷不依賴幾何。
//
// 與 engine/redaction/content_redactor.cpp 的 ContentLexer 是同一套規則的
// 精簡版，沒有直接重用是因為那個類別是該檔案匿名命名空間裡的私有型別，
// 兩邊要解的問題也不同（一個要重放圖形狀態算座標，一個只要正確識別
// BDC/BMC/EMC/DP/MP/Do 的邊界）。
class MarkedContentLexer {
public:
    explicit MarkedContentLexer(const std::string& bytes) : bytes_(bytes) {}

    [[nodiscard]] Token next() {
        skipTrivia();
        Token token;
        token.begin = pos_;
        if (pos_ >= bytes_.size()) {
            token.end = pos_;
            return token;
        }

        const char c = bytes_[pos_];
        switch (c) {
            case '[': ++pos_; token.kind = TokenKind::ArrayStart; token.end = pos_; return token;
            case ']': ++pos_; token.kind = TokenKind::ArrayEnd;   token.end = pos_; return token;
            case '(': return literalString(token);
            case '/': return name(token);
            case '<':
                if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '<') {
                    pos_ += 2;
                    token.kind = TokenKind::DictStart;
                    token.end = pos_;
                    return token;
                }
                return hexString(token);
            case '>':
                if (pos_ + 1 < bytes_.size() && bytes_[pos_ + 1] == '>') {
                    pos_ += 2;
                    token.kind = TokenKind::DictEnd;
                    token.end = pos_;
                    return token;
                }
                ++pos_;
                token.kind = TokenKind::Operator;
                token.end = pos_;
                return token;
            default: break;
        }

        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') return number(token);
        return oper(token);
    }

    void seek(std::size_t position) noexcept { pos_ = position; }

private:
    void skipTrivia() {
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c)) {
                ++pos_;
                continue;
            }
            if (c == '%') {
                while (pos_ < bytes_.size() && bytes_[pos_] != '\n' && bytes_[pos_] != '\r') ++pos_;
                continue;
            }
            return;
        }
    }

    // 字串與十六進位字串的內容對可見性判斷沒有意義，這裡只需要正確跳過括號
    // 配對與跳脫，不保留解碼後的位元組。
    Token literalString(Token token) {
        token.kind = TokenKind::String;
        ++pos_;  // '('
        int depth = 1;
        while (pos_ < bytes_.size() && depth > 0) {
            const char c = bytes_[pos_++];
            if (c == '\\') {
                if (pos_ < bytes_.size()) ++pos_;
                continue;
            }
            if (c == '(') { ++depth; continue; }
            if (c == ')') {
                if (--depth == 0) break;
                continue;
            }
        }
        token.end = pos_;
        return token;
    }

    Token hexString(Token token) {
        token.kind = TokenKind::String;
        ++pos_;  // '<'
        while (pos_ < bytes_.size() && bytes_[pos_] != '>') ++pos_;
        if (pos_ < bytes_.size()) ++pos_;  // '>'
        token.end = pos_;
        return token;
    }

    Token name(Token token) {
        token.kind = TokenKind::Name;
        ++pos_;  // '/'
        std::string out;
        while (pos_ < bytes_.size()) {
            const auto c = static_cast<unsigned char>(bytes_[pos_]);
            if (isWhitespaceByte(c) || isDelimiterByte(c)) break;
            if (c == '#' && pos_ + 2 < bytes_.size()) {
                const auto hex = bytes_.substr(pos_ + 1, 2);
                char* stop = nullptr;
                const long value = std::strtol(hex.c_str(), &stop, 16);
                if (stop == hex.c_str() + 2) {
                    out += static_cast<char>(value);
                    pos_ += 3;
                    continue;
                }
            }
            out += static_cast<char>(c);
            ++pos_;
        }
        token.text = std::move(out);
        token.end = pos_;
        return token;
    }

    Token number(Token token) {
        token.kind = TokenKind::Number;
        while (pos_ < bytes_.size()) {
            const char c = bytes_[pos_];
            if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
                ++pos_;
                continue;
            }
            break;
        }
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
        if (pos_ == start) ++pos_;  // 無法辨識的單一位元組，前進以免死迴圈
        token.text = bytes_.substr(start, pos_ - start);
        token.end = pos_;
        return token;
    }

    const std::string& bytes_;
    std::size_t pos_{0};
};

// BI …（字典）… ID <二進位> EI。二進位資料可能包含任何位元組，結束標記
// 必須以「前後都是分隔符的 EI」判定，不能直接找字串 "EI"。
std::size_t skipInlineImage(const std::string& content, std::size_t afterBI) {
    std::size_t pos = afterBI;
    while (pos + 1 < content.size()) {
        if (content[pos] == 'I' && content[pos + 1] == 'D') {
            pos += 2;
            break;
        }
        ++pos;
    }
    if (pos < content.size() && isWhitespaceByte(static_cast<unsigned char>(content[pos]))) ++pos;
    while (pos + 1 < content.size()) {
        if (content[pos] == 'E' && content[pos + 1] == 'I') {
            const bool beforeOk =
                pos == 0 || isWhitespaceByte(static_cast<unsigned char>(content[pos - 1]));
            const bool afterOk =
                pos + 2 >= content.size() ||
                isWhitespaceByte(static_cast<unsigned char>(content[pos + 2])) ||
                isDelimiterByte(static_cast<unsigned char>(content[pos + 2]));
            if (beforeOk && afterOk) return pos + 2;
        }
        ++pos;
    }
    return content.size();
}

struct OcResolution {
    bool visible{true};
    bool sawVisibilityExpression{false};
};

// 判斷一個 /OC 項目（指向 OCG 或 OCMD 的間接參照）目前是否可見。
//
// 找不到、解不出字典、或不是合規的間接參照，一律保守回傳 visible = true——
// 攤平不可逆，錯誤的方向只能是「留下不該留的」而不是「刪掉不該刪的」。
OcResolution resolveOcVisibility(PdfDocumentRewriter& document, const domain::OcgTree& tree,
                                 const PdfObject& ocEntry) {
    OcResolution result;
    if (!ocEntry.isRef()) return result;  // 規範要求間接參照，內嵌字典不合規

    const int number = ocEntry.asRef().number;
    const PdfObject resolved = document.resolve(ocEntry);
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return result;

    const PdfObject* type = dict->find("Type");
    const bool isOcmd = type != nullptr && type->isName("OCMD");
    if (!isOcmd) {
        // 一般 OCG：tree 裡的 objectNumber 就是它的間接物件編號
        // （engine/layers/ocg_reader.cpp 解析 /OCGs 時的填法）。
        const domain::OcgLayer* layer = tree.find(number);
        result.visible = layer == nullptr ? true : layer->visible;
        return result;
    }

    if (dict->has("VE")) {
        // /VE 可見性運算式：目前不求值，保守視為可見並讓呼叫端知道發生過這件事。
        result.sawVisibilityExpression = true;
        result.visible = true;
        return result;
    }

    std::vector<bool> memberVisible;
    if (const PdfObject* ocgs = dict->find("OCGs")) {
        const PdfObject resolvedOcgs = document.resolve(*ocgs);
        if (const PdfArray* array = resolvedOcgs.asArray()) {
            for (const PdfObject& item : *array) {
                if (!item.isRef()) continue;
                const domain::OcgLayer* layer = tree.find(item.asRef().number);
                memberVisible.push_back(layer == nullptr ? true : layer->visible);
            }
        } else if (ocgs->isRef()) {
            // /OCGs 也允許單一（非陣列）間接參照。
            const domain::OcgLayer* layer = tree.find(ocgs->asRef().number);
            memberVisible.push_back(layer == nullptr ? true : layer->visible);
        }
    }

    domain::OcmdPolicy policy = domain::OcmdPolicy::AnyOn;
    if (const PdfObject* p = dict->find("P")) {
        const std::string policyName = p->asName();
        if (policyName == "AllOn") policy = domain::OcmdPolicy::AllOn;
        else if (policyName == "AnyOff") policy = domain::OcmdPolicy::AnyOff;
        else if (policyName == "AllOff") policy = domain::OcmdPolicy::AllOff;
    }
    result.visible = domain::resolveOcmdPolicy(policy, memberVisible);
    return result;
}

// 在 resources 的 /<category> 子字典裡找一個名稱對應的項目。
// storage 是呼叫端提供的暫存空間：回傳的指標指向它內部，呼叫端必須讓
// storage 活得比回傳指標久。
const PdfObject* lookupResourceEntry(PdfDocumentRewriter& document, const PdfObject& resources,
                                     const std::string& category, const std::string& name,
                                     PdfObject& storage) {
    const PdfObject resolvedResources = document.resolve(resources);
    const PdfDictionary* resDict = resolvedResources.asDictionary();
    if (resDict == nullptr) return nullptr;
    const PdfObject* categoryEntry = resDict->find(category);
    if (categoryEntry == nullptr) return nullptr;
    storage = document.resolve(*categoryEntry);
    const PdfDictionary* categoryDict = storage.asDictionary();
    if (categoryDict == nullptr) return nullptr;
    return categoryDict->find(name);
}

int removeHiddenAnnotations(PdfDocumentRewriter& document, const domain::OcgTree& tree,
                            int pageObject, bool& sawVisibilityExpression) {
    PdfArray* annots = redaction::unsharedDictionaryArray(document, pageObject, "Annots");
    if (annots == nullptr) return 0;

    PdfArray kept;
    int removed = 0;
    for (const PdfObject& entry : *annots) {
        const PdfObject annotation = document.resolve(entry);
        const PdfDictionary* dict = annotation.asDictionary();
        if (dict == nullptr) {
            kept.push_back(entry);
            continue;
        }
        const PdfObject* oc = dict->find("OC");
        if (oc == nullptr) {
            kept.push_back(entry);
            continue;
        }
        const OcResolution resolution = resolveOcVisibility(document, tree, *oc);
        if (resolution.sawVisibilityExpression) sawVisibilityExpression = true;
        if (!resolution.visible) {
            ++removed;
            continue;
        }
        kept.push_back(entry);
    }
    if (removed > 0) *annots = std::move(kept);
    return removed;
}

struct PageContent {
    std::string decoded{};
    bool hasContent{false};
};

// 收集頁面的內容串流並解碼成一段連續位元組。/Contents 是陣列時各段語意上
// 等同單一串流（ISO 32000-1 §7.8.2），合併後編輯是對的。
//
// 與 engine/redaction 的同名函式不同：這裡不檢查串流物件是否被多個頁面
// 共用而拒絕處理，因為攤平永遠會產生一個新的內容串流物件並把該頁的
// /Contents 改指過去，不會就地覆寫原物件——即使原物件真的被兩個頁面共用，
// 另一頁仍然會在輪到它時，依它自己當時的 /Contents 重新走一次同樣的流程，
// 不會因為共用而漏改或改錯邊。
bool collectPageContent(PdfDocumentRewriter& document, const PdfDictionary& page, PageContent& out,
                        std::string& diagnostic) {
    const PdfObject* contents = page.find("Contents");
    if (contents == nullptr) return true;

    std::vector<PdfObject> entries;
    if (contents->isRef()) {
        const PdfObject* target = document.object(contents->asRef().number);
        if (target == nullptr) return true;
        if (target->asArray() != nullptr) {
            for (const PdfObject& item : *target->asArray()) entries.push_back(item);
        } else {
            entries.push_back(*contents);
        }
    } else if (const PdfArray* array = contents->asArray()) {
        for (const PdfObject& item : *array) entries.push_back(item);
    }

    for (const PdfObject& entry : entries) {
        if (!entry.isRef()) continue;
        const int number = entry.asRef().number;
        const PdfObject* target = document.object(number);
        if (target == nullptr) continue;
        const PdfStream* stream = target->asStream();
        if (stream == nullptr) continue;

        const objects::DecodeResult decoded =
            objects::decodeStream(*stream, [&document](const PdfRef& ref) {
                const PdfObject* found = document.object(ref.number);
                return found == nullptr ? PdfObject{} : *found;
            });
        if (!decoded.ok) {
            diagnostic = "內容串流物件 " + std::to_string(number) +
                        " 的濾鏡無法解開，因此無法確認要移除哪些隱藏圖層內容：" +
                        decoded.diagnostic;
            return false;
        }
        out.hasContent = true;
        if (!out.decoded.empty()) out.decoded += '\n';
        out.decoded += decoded.data;
    }
    return true;
}

class LayerFlattener {
public:
    LayerFlattener(PdfDocumentRewriter& document, const domain::OcgTree& tree)
        : document_(document), tree_(tree) {}

    // 攤平一段內容串流（頁面內容，或遞迴進入的 Form XObject 內容）。
    // 失敗時回傳 false 並填 diagnostic；呼叫端必須整個放棄輸出，
    // 不得只跳過這一段——標記內容巢狀不平衡代表輸入已經不可信。
    bool flatten(const std::string& content, const PdfObject& resources, int depth,
                std::string& out, FlattenStats& stats, std::string& diagnostic) {
        if (depth > kMaxFormDepth) {
            diagnostic = "Form XObject 巢狀過深，拒絕繼續走訪";
            return false;
        }

        MarkedContentLexer lexer(content);
        std::vector<Token> operands;
        struct McFrame {
            bool hidden{false};
        };
        std::vector<McFrame> stack;
        std::size_t copied = 0;
        std::string result;

        while (true) {
            Token token = lexer.next();
            if (token.kind == TokenKind::End) break;
            if (token.kind != TokenKind::Operator) {
                operands.push_back(std::move(token));
                continue;
            }

            const bool currentlyHidden = !stack.empty() && stack.back().hidden;
            const std::size_t operandStart = operands.empty() ? token.begin : operands.front().begin;

            if (token.text == "BI") {
                // 內嵌影像的二進位資料會讓語彙分析錯亂，必須整段跳過。
                const std::size_t endOfImage = skipInlineImage(content, token.end);
                if (currentlyHidden) {
                    result.append(content, copied, token.begin - copied);
                    copied = endOfImage;
                }
                lexer.seek(endOfImage);
                operands.clear();
                continue;
            }

            if (token.text == "BDC" || token.text == "BMC") {
                bool childHidden = currentlyHidden;
                if (token.text == "BDC" && operands.size() == 2 &&
                    operands[0].kind == TokenKind::Name && operands[0].text == "OC" &&
                    operands[1].kind == TokenKind::Name) {
                    PdfObject storage;
                    const PdfObject* entry = lookupResourceEntry(document_, resources, "Properties",
                                                                 operands[1].text, storage);
                    if (entry != nullptr) {
                        const OcResolution resolution = resolveOcVisibility(document_, tree_, *entry);
                        if (resolution.sawVisibilityExpression) sawVisibilityExpression_ = true;
                        childHidden = currentlyHidden || !resolution.visible;
                    }
                    // entry == nullptr（找不到 /Properties 對應項目）：保守維持
                    // currentlyHidden，既不額外隱藏也不額外顯示。
                }
                if (childHidden && !currentlyHidden) ++stats.removedMarkedContentSpans;
                if (childHidden) {
                    result.append(content, copied, operandStart - copied);
                    copied = token.end;
                }
                stack.push_back(McFrame{childHidden});
                operands.clear();
                continue;
            }

            if (token.text == "EMC") {
                if (stack.empty()) {
                    diagnostic = "內容串流的 EMC 沒有對應的 BDC/BMC，標記內容巢狀不平衡";
                    return false;
                }
                const bool poppedHidden = stack.back().hidden;
                stack.pop_back();
                if (poppedHidden) {
                    result.append(content, copied, operandStart - copied);
                    copied = token.end;
                }
                operands.clear();
                continue;
            }

            if (token.text == "Do" && !operands.empty() && operands.back().kind == TokenKind::Name) {
                const std::string xobjectName = operands.back().text;
                if (currentlyHidden) {
                    result.append(content, copied, operandStart - copied);
                    copied = token.end;
                    operands.clear();
                    continue;
                }
                PdfObject storage;
                const PdfObject* entry =
                    lookupResourceEntry(document_, resources, "XObject", xobjectName, storage);
                if (entry != nullptr && entry->isRef()) {
                    const int number = entry->asRef().number;
                    const PdfObject* target = document_.object(number);
                    const PdfDictionary* dict = target == nullptr ? nullptr : target->asDictionary();
                    if (dict != nullptr) {
                        bool skip = false;
                        if (const PdfObject* oc = dict->find("OC")) {
                            const OcResolution resolution = resolveOcVisibility(document_, tree_, *oc);
                            if (resolution.sawVisibilityExpression) sawVisibilityExpression_ = true;
                            skip = !resolution.visible;
                        }
                        if (skip) {
                            ++stats.removedXObjectDraws;
                            result.append(content, copied, operandStart - copied);
                            copied = token.end;
                            operands.clear();
                            continue;
                        }
                        const PdfObject* subtype = dict->find("Subtype");
                        if (subtype != nullptr && subtype->isName("Form")) {
                            if (!flattenFormOnce(number, resources, depth, stats, diagnostic)) {
                                return false;
                            }
                        }
                    }
                }
                operands.clear();
                continue;
            }

            // 其餘所有運算子（含 DP、MP：它們是點標記內容，不參與巢狀計數，
            // 但仍要依目前的可見性狀態決定去留）一視同仁：隱藏就整段拿掉。
            if (currentlyHidden) {
                result.append(content, copied, operandStart - copied);
                copied = token.end;
            }
            operands.clear();
        }

        if (!stack.empty()) {
            diagnostic = "內容串流結束時仍有未關閉的 BDC/BMC，標記內容巢狀不平衡";
            return false;
        }

        result.append(content, copied, std::string::npos);
        out = std::move(result);
        return true;
    }

    [[nodiscard]] bool sawVisibilityExpression() const noexcept { return sawVisibilityExpression_; }

private:
    // 攤平一個 Form XObject 的內部內容，去重（同一物件只處理一次）。
    //
    // 可見性只取決於文件全域的 OCG 狀態，不取決於哪個頁面呼叫它，因此被多個
    // 頁面共用的 Form 可以放心就地改寫一次。唯一的例外是 Form 自己沒有
    // /Resources、必須沿用呼叫端資源字典的情況——那時攤平結果理論上可能
    // 隨呼叫端而異（不同呼叫端的 /Properties、/XObject 名稱映射可能不同），
    // 為了不要在不確定的情況下寫出可能錯誤的結果，第二次遇到就明確失敗，
    // 而不是沿用第一次呼叫端的資源去覆蓋，那會靜默套用錯誤的映射。
    bool flattenFormOnce(int number, const PdfObject& callerResources, int depth,
                        FlattenStats& stats, std::string& diagnostic) {
        const auto already = processedForms_.find(number);
        if (already != processedForms_.end()) {
            if (already->second) return true;
            diagnostic = "Form XObject " + std::to_string(number) +
                        " 沒有自己的 /Resources、依賴呼叫端繼承而來，且被多處呼叫，"
                        "無法確定要用哪一份資源字典重新攤平，拒絕產生可能不一致的輸出";
            return false;
        }

        PdfObject* target = document_.object(number);
        PdfStream* stream = target == nullptr ? nullptr : target->asStream();
        if (stream == nullptr) {
            processedForms_[number] = true;
            return true;
        }

        const PdfObject* ownResources = stream->dict.find("Resources");
        const PdfObject resources = ownResources != nullptr ? *ownResources : callerResources;
        processedForms_[number] = ownResources != nullptr;

        const objects::DecodeResult decoded =
            objects::decodeStream(*stream, [this](const PdfRef& ref) {
                const PdfObject* found = document_.object(ref.number);
                return found == nullptr ? PdfObject{} : *found;
            });
        if (!decoded.ok) {
            diagnostic = "Form XObject " + std::to_string(number) +
                        " 的濾鏡無法解開，因此無法確認其中是否有要移除的隱藏圖層內容：" +
                        decoded.diagnostic;
            return false;
        }

        std::string flattened;
        if (!flatten(decoded.data, resources, depth + 1, flattened, stats, diagnostic)) return false;

        PdfObject* mutableTarget = document_.object(number);
        PdfStream* mutableStream = mutableTarget->asStream();
        mutableStream->data = flattened;
        // 編輯後的位元組是未壓縮的，濾鏡宣告必須跟著拿掉，否則解析器會把
        // 明文當成 Flate 資料而整段解析失敗。
        mutableStream->dict.remove("Filter");
        mutableStream->dict.remove("DecodeParms");
        ++stats.editedForms;
        return true;
    }

    PdfDocumentRewriter& document_;
    const domain::OcgTree& tree_;
    std::map<int, bool> processedForms_{};  // objectNumber -> 是否有自己的 /Resources
    bool sawVisibilityExpression_{false};
};

}  // namespace

FlattenResult flattenLayers(std::string sourceBytes, const domain::OcgTree& tree,
                            domain::IrreversibleConsent) {
    FlattenResult result;
    if (!tree.present) {
        result.diagnostic = "文件沒有 /OCProperties，沒有圖層可以攤平";
        return result;
    }

    PdfDocumentRewriter document;
    std::string openDiagnostic;
    const objects::SourceStatus status = document.open(std::move(sourceBytes), &openDiagnostic);
    if (status != objects::SourceStatus::Ok) {
        result.diagnostic = std::string{objects::describe(status)};
        if (status == objects::SourceStatus::Encrypted) {
            result.diagnostic =
                "加密文件不支援圖層攤平：字串與串流需要加密後才寫得回去，"
                "半套的實作會產出看起來成功但讀不出來的檔案";
        }
        if (!openDiagnostic.empty()) result.diagnostic += "：" + openDiagnostic;
        return result;
    }

    LayerFlattener flattener(document, tree);
    const int pageCount = document.pageCount();
    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        PdfRef pageRef{};
        if (!document.pageRef(pageIndex, pageRef)) {
            result.diagnostic = "頁碼超出範圍：" + std::to_string(pageIndex);
            return result;
        }
        const int pageObject = pageRef.number;
        const PdfObject* page = document.object(pageObject);
        if (page == nullptr || page->asDictionary() == nullptr) {
            result.diagnostic = "頁面物件不是字典：" + std::to_string(pageObject);
            return result;
        }

        PageContent content;
        if (!collectPageContent(document, *page->asDictionary(), content, result.diagnostic)) {
            return result;
        }

        if (content.hasContent) {
            const PdfObject resources = document.inheritedPageAttribute(pageRef, "Resources");
            std::string flattened;
            if (!flattener.flatten(content.decoded, resources, 0, flattened, result.stats,
                                   result.diagnostic)) {
                return result;
            }
            PdfDictionary contentDict;
            const int contentObject =
                document.addObject(PdfObject{PdfStream{std::move(contentDict), std::move(flattened)}});
            PdfObject* mutablePage = document.object(pageObject);
            mutablePage->asDictionary()->set("Contents", objects::makeRef(contentObject));
        }

        bool sawVisibilityExpression = false;
        result.stats.removedAnnotations +=
            removeHiddenAnnotations(document, tree, pageObject, sawVisibilityExpression);
        if (sawVisibilityExpression) result.sawUnsupportedVisibilityExpression = true;

        ++result.stats.pagesTouched;
    }

    if (flattener.sawVisibilityExpression()) result.sawUnsupportedVisibilityExpression = true;

    result.bytes = document.build();
    result.ok = true;
    return result;
}

}  // namespace alioth::engine::layers
