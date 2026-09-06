#include "engine/forms/form_data.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace alioth::engine::forms {
namespace {

const std::string kEmpty;

[[nodiscard]] bool isAsciiPrintable(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x20 && u < 0x7F;
    });
}

// UTF-8 → UTF-16BE 位元組。FDF 的非 ASCII 值一律走 <FEFF...> 十六進位字串，
// 因為 PDF 的字面字串沒有編碼宣告，直接塞 UTF-8 位元組在 Acrobat 會被
// 當成 PDFDocEncoding 解讀，中文姓名會變成看似隨機的拉丁字母。
[[nodiscard]] std::vector<std::uint8_t> utf8ToUtf16Be(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t code = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code = lead & 0x07u;
            extra = 3;
        } else {
            // 非法前導位元組。以 U+FFFD 取代而不是中止：匯出不該因為一個
            // 壞字元就整份失敗，但也不能靜默丟字。
            code = 0xFFFD;
        }
        if (extra > 0 && i + extra < text.size()) {
            for (std::size_t k = 1; k <= extra; ++k) {
                code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            }
            i += extra;
        } else if (extra > 0) {
            code = 0xFFFD;
        }
        ++i;

        if (code >= 0x10000) {
            const char32_t v = code - 0x10000;
            const std::uint16_t hi = static_cast<std::uint16_t>(0xD800 + (v >> 10));
            const std::uint16_t lo = static_cast<std::uint16_t>(0xDC00 + (v & 0x3FF));
            out.push_back(static_cast<std::uint8_t>(hi >> 8));
            out.push_back(static_cast<std::uint8_t>(hi & 0xFF));
            out.push_back(static_cast<std::uint8_t>(lo >> 8));
            out.push_back(static_cast<std::uint8_t>(lo & 0xFF));
        } else {
            out.push_back(static_cast<std::uint8_t>(code >> 8));
            out.push_back(static_cast<std::uint8_t>(code & 0xFF));
        }
    }
    return out;
}

[[nodiscard]] std::string utf16BeToUtf8(const std::vector<std::uint8_t>& bytes,
                                        std::size_t startIndex) {
    std::string out;
    for (std::size_t i = startIndex; i + 1 < bytes.size(); i += 2) {
        char32_t code = static_cast<char32_t>((bytes[i] << 8) | bytes[i + 1]);
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < bytes.size()) {
            const char32_t low = static_cast<char32_t>((bytes[i + 2] << 8) | bytes[i + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

[[nodiscard]] std::string toPdfString(std::string_view text) {
    if (isAsciiPrintable(text)) {
        std::string out = "(";
        for (const char c : text) {
            if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back(')');
        return out;
    }
    static const char* kHex = "0123456789ABCDEF";
    std::string out = "<FEFF";
    for (const std::uint8_t b : utf8ToUtf16Be(text)) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    out.push_back('>');
    return out;
}

[[nodiscard]] std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

[[nodiscard]] std::string xmlUnescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        const std::size_t end = text.find(';', i);
        if (end == std::string_view::npos) {
            out.push_back('&');
            continue;
        }
        const std::string_view entity = text.substr(i + 1, end - i - 1);
        if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            // 數值實體。XFDF 由其他工具產生時很常見，不處理會讓值被整段吞掉。
            unsigned long code = 0;
            if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
                for (std::size_t k = 2; k < entity.size(); ++k) {
                    const char c = entity[k];
                    const int digit = (c >= '0' && c <= '9')   ? c - '0'
                                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                               : -1;
                    if (digit < 0) { code = 0; break; }
                    code = code * 16 + static_cast<unsigned long>(digit);
                }
            } else {
                for (std::size_t k = 1; k < entity.size(); ++k) {
                    if (entity[k] < '0' || entity[k] > '9') { code = 0; break; }
                    code = code * 10 + static_cast<unsigned long>(entity[k] - '0');
                }
            }
            const auto cp = static_cast<char32_t>(code);
            if (cp == 0) {
                out.push_back('&');
                out.append(entity);
                out.push_back(';');
            } else if (cp < 0x80) {
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
        } else {
            out.push_back('&');
            out.append(entity);
            out.push_back(';');
        }
        i = end;
    }
    return out;
}

// 從 pos 起解析一個 PDF 字串（字面或十六進位），回傳解出的 UTF-8 與結束位置。
struct ParsedString {
    bool ok{false};
    std::string value;
    std::size_t next{0};
};

[[nodiscard]] ParsedString parsePdfString(std::string_view text, std::size_t pos) {
    ParsedString result;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size()) return result;

    if (text[pos] == '(') {
        std::string out;
        int depth = 1;
        ++pos;
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == '\\') {
                if (pos + 1 >= text.size()) break;
                out.push_back(text[pos + 1]);
                pos += 2;
                continue;
            }
            if (c == '(') ++depth;
            if (c == ')') {
                if (--depth == 0) { ++pos; break; }
            }
            out.push_back(c);
            ++pos;
        }
        if (depth != 0) return result;
        result.ok = true;
        result.value = std::move(out);
        result.next = pos;
        return result;
    }

    if (text[pos] == '<') {
        std::vector<std::uint8_t> bytes;
        int high = -1;
        ++pos;
        for (; pos < text.size() && text[pos] != '>'; ++pos) {
            const char c = text[pos];
            const int digit = (c >= '0' && c <= '9')   ? c - '0'
                              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                       : -1;
            if (digit < 0) continue;
            if (high < 0) {
                high = digit;
            } else {
                bytes.push_back(static_cast<std::uint8_t>((high << 4) | digit));
                high = -1;
            }
        }
        if (pos >= text.size()) return result;
        if (high >= 0) bytes.push_back(static_cast<std::uint8_t>(high << 4));
        ++pos;
        result.ok = true;
        if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
            result.value = utf16BeToUtf8(bytes, 2);
        } else {
            result.value.assign(bytes.begin(), bytes.end());
        }
        result.next = pos;
        return result;
    }

    return result;
}

}  // namespace

const std::string& FormDataEntry::primary() const {
    return values.empty() ? kEmpty : values.front();
}

bool isExportable(const FormFieldInfo& field) {
    if (field.flags.noExport) return false;
    if (field.type == FormFieldType::PushButton) return false;
    // 簽章值搬到另一份文件沒有意義，而且會讓收檔的人以為那份也被簽過。
    if (field.type == FormFieldType::Signature) return false;
    if (field.type == FormFieldType::Unknown || field.type == FormFieldType::Xfa) return false;
    if (field.name.empty()) return false;
    return true;
}

std::string exportFormData(const std::vector<FormFieldInfo>& fields, FormDataFormat format,
                           std::string_view sourcePath) {
    // 單選群組的每個 widget 都帶同一個欄位名與同一個群組值，逐個寫出會產生重複項。
    // 以名稱去重，先出現者為準。
    std::vector<const FormFieldInfo*> unique;
    for (const auto& field : fields) {
        if (!isExportable(field)) continue;
        const bool seen = std::any_of(unique.begin(), unique.end(), [&](const FormFieldInfo* f) {
            return f->name == field.name;
        });
        if (!seen) unique.push_back(&field);
    }

    if (format == FormDataFormat::Fdf) {
        std::string body;
        body += "%FDF-1.2\n%\xE2\xE3\xCF\xD3\n1 0 obj\n<< /FDF << /Fields [\n";
        for (const FormFieldInfo* field : unique) {
            body += "<< /T " + toPdfString(field->name) + " /V ";
            if (field->isChoice() && field->selectedIndices.size() > 1) {
                body += "[";
                for (const std::int32_t index : field->selectedIndices) {
                    if (index >= 0 && index < static_cast<std::int32_t>(field->options.size())) {
                        body += " " + toPdfString(field->options[static_cast<std::size_t>(index)]);
                    }
                }
                body += " ]";
            } else if (field->isButton()) {
                // 核取／單選寫的是狀態名（/Yes、/Off），不是顯示文字。
                body += "/" + (field->checked && !field->exportValue.empty() ? field->exportValue
                                                                            : std::string("Off"));
            } else {
                body += toPdfString(field->value);
            }
            body += " >>\n";
        }
        body += "]";
        if (!sourcePath.empty()) body += " /F " + toPdfString(sourcePath);
        body += " >> >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n";
        return body;
    }

    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n";
    if (!sourcePath.empty()) {
        out += "  <f href=\"" + xmlEscape(sourcePath) + "\"/>\n";
    }
    out += "  <fields>\n";
    for (const FormFieldInfo* field : unique) {
        out += "    <field name=\"" + xmlEscape(field->name) + "\">\n";
        if (field->isChoice() && field->selectedIndices.size() > 1) {
            for (const std::int32_t index : field->selectedIndices) {
                if (index >= 0 && index < static_cast<std::int32_t>(field->options.size())) {
                    out += "      <value>" +
                           xmlEscape(field->options[static_cast<std::size_t>(index)]) +
                           "</value>\n";
                }
            }
        } else if (field->isButton()) {
            const std::string state =
                field->checked && !field->exportValue.empty() ? field->exportValue : "Off";
            out += "      <value>" + xmlEscape(state) + "</value>\n";
        } else {
            out += "      <value>" + xmlEscape(field->value) + "</value>\n";
        }
        out += "    </field>\n";
    }
    out += "  </fields>\n</xfdf>\n";
    return out;
}

std::optional<FormDataFormat> detectFormat(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const std::string_view rest = text.substr(i);
    if (rest.rfind("%FDF", 0) == 0) return FormDataFormat::Fdf;
    if (rest.rfind("<?xml", 0) == 0 || rest.rfind("<xfdf", 0) == 0) return FormDataFormat::Xfdf;
    return std::nullopt;
}

namespace {

FormDataImport importFdf(std::string_view text) {
    FormDataImport result;
    const std::size_t fieldsPos = text.find("/Fields");
    if (text.find("%FDF") == std::string_view::npos || fieldsPos == std::string_view::npos) {
        result.error = "不是有效的 FDF：找不到 %FDF 標頭或 /Fields 陣列。";
        return result;
    }

    std::size_t pos = fieldsPos;
    while (true) {
        const std::size_t tPos = text.find("/T", pos);
        if (tPos == std::string_view::npos) break;
        const ParsedString name = parsePdfString(text, tPos + 2);
        if (!name.ok) {
            result.error = "FDF 欄位名稱解析失敗，檔案可能已損毀。";
            return result;
        }

        FormDataEntry entry;
        entry.name = name.value;

        const std::size_t vPos = text.find("/V", name.next);
        const std::size_t nextT = text.find("/T", name.next);
        if (vPos != std::string_view::npos && (nextT == std::string_view::npos || vPos < nextT)) {
            std::size_t cursor = vPos + 2;
            while (cursor < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor < text.size() && text[cursor] == '[') {
                ++cursor;
                while (cursor < text.size() && text[cursor] != ']') {
                    const ParsedString item = parsePdfString(text, cursor);
                    if (!item.ok) break;
                    entry.values.push_back(item.value);
                    cursor = item.next;
                    while (cursor < text.size() &&
                           std::isspace(static_cast<unsigned char>(text[cursor]))) {
                        ++cursor;
                    }
                }
            } else if (cursor < text.size() && text[cursor] == '/') {
                ++cursor;
                std::string name2;
                while (cursor < text.size() && !std::isspace(static_cast<unsigned char>(text[cursor])) &&
                       text[cursor] != '/' && text[cursor] != '>' && text[cursor] != ']') {
                    name2.push_back(text[cursor]);
                    ++cursor;
                }
                entry.values.push_back(name2);
            } else {
                const ParsedString value = parsePdfString(text, cursor);
                if (value.ok) entry.values.push_back(value.value);
            }
        }
        if (entry.values.empty()) entry.values.emplace_back();
        result.entries.push_back(std::move(entry));
        pos = name.next;
    }

    result.ok = true;
    return result;
}

FormDataImport importXfdf(std::string_view text) {
    FormDataImport result;
    if (text.find("<xfdf") == std::string_view::npos) {
        result.error = "不是有效的 XFDF：找不到 <xfdf> 根元素。";
        return result;
    }

    std::size_t pos = 0;
    while (true) {
        const std::size_t fieldPos = text.find("<field", pos);
        if (fieldPos == std::string_view::npos) break;
        const std::size_t tagEnd = text.find('>', fieldPos);
        if (tagEnd == std::string_view::npos) {
            result.error = "XFDF 的 <field> 標籤未正確結束。";
            return result;
        }
        const std::string_view tag = text.substr(fieldPos, tagEnd - fieldPos);
        const std::size_t namePos = tag.find("name=");
        if (namePos == std::string_view::npos) {
            pos = tagEnd + 1;
            continue;
        }
        const char quote = tag[namePos + 5];
        const std::size_t nameStart = namePos + 6;
        const std::size_t nameEnd = tag.find(quote, nameStart);
        if (nameEnd == std::string_view::npos) {
            result.error = "XFDF 的 name 屬性未正確結束。";
            return result;
        }

        FormDataEntry entry;
        entry.name = xmlUnescape(tag.substr(nameStart, nameEnd - nameStart));

        // 只掃到下一個 <field 為止，避免把後面欄位的值算到這一筆上。
        const std::size_t limit = text.find("<field", tagEnd);
        std::size_t cursor = tagEnd;
        while (true) {
            const std::size_t valuePos = text.find("<value", cursor);
            if (valuePos == std::string_view::npos) break;
            if (limit != std::string_view::npos && valuePos > limit) break;
            const std::size_t valueTagEnd = text.find('>', valuePos);
            if (valueTagEnd == std::string_view::npos) break;
            if (valueTagEnd > 0 && text[valueTagEnd - 1] == '/') {
                entry.values.emplace_back();
                cursor = valueTagEnd + 1;
                continue;
            }
            const std::size_t closePos = text.find("</value>", valueTagEnd);
            if (closePos == std::string_view::npos) break;
            entry.values.push_back(
                xmlUnescape(text.substr(valueTagEnd + 1, closePos - valueTagEnd - 1)));
            cursor = closePos + 8;
        }
        if (entry.values.empty()) entry.values.emplace_back();
        result.entries.push_back(std::move(entry));
        pos = tagEnd + 1;
    }

    result.ok = true;
    return result;
}

}  // namespace

FormDataImport importFormData(std::string_view text, FormDataFormat format) {
    return format == FormDataFormat::Fdf ? importFdf(text) : importXfdf(text);
}

}  // namespace alioth::engine::forms
