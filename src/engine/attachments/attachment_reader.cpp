#include "engine/attachments/attachment_reader.h"

#include <algorithm>
#include <cctype>
#include <variant>

#include "engine/objects/pdf_parser.h"

namespace alioth::engine::attachments {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::PdfStream;
using objects::PdfString;

constexpr int kMaxNameTreeDepth = 32;

std::string textOf(const PdfSourceDocument& source, const PdfDictionary& dict, const char* key) {
    const PdfObject* value = dict.find(key);
    if (value == nullptr) return {};
    const PdfObject resolved = source.resolve(*value);
    if (const PdfString* text = std::get_if<PdfString>(&resolved.value())) return text->bytes;
    return {};
}

void collectNameTree(const PdfSourceDocument& source, const PdfObject& node, int depth,
                     std::vector<std::pair<std::string, PdfObject>>& out) {
    if (depth >= kMaxNameTreeDepth) return;
    const PdfObject resolved = source.resolve(node);
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return;

    if (const PdfObject* names = dict->find("Names")) {
        const PdfObject resolvedNames = source.resolve(*names);
        if (const PdfArray* array = resolvedNames.asArray()) {
            for (std::size_t i = 0; i + 1 < array->size(); i += 2) {
                const PdfObject key = source.resolve((*array)[i]);
                if (const PdfString* text = std::get_if<PdfString>(&key.value())) {
                    out.emplace_back(text->bytes, (*array)[i + 1]);
                }
            }
        }
    }
    if (const PdfObject* kids = dict->find("Kids")) {
        const PdfObject resolvedKids = source.resolve(*kids);
        if (const PdfArray* array = resolvedKids.asArray()) {
            for (const PdfObject& kid : *array) collectNameTree(source, kid, depth + 1, out);
        }
    }
}

// /EF << /F 12 0 R >>：附件的實際位元組所在。/UF 是 Unicode 版本，
// 兩者指向同一份資料時任取其一即可。
int embeddedStreamObject(const PdfSourceDocument& source, const PdfDictionary& fileSpec) {
    const PdfObject* ef = fileSpec.find("EF");
    if (ef == nullptr) return 0;
    const PdfObject resolvedEf = source.resolve(*ef);
    const PdfDictionary* efDict = resolvedEf.asDictionary();
    if (efDict == nullptr) return 0;
    for (const char* key : {"F", "UF", "DOS", "Mac", "Unix"}) {
        if (const PdfObject* stream = efDict->find(key)) {
            if (stream->isRef()) return stream->asRef().number;
        }
    }
    return 0;
}

void fillFromFileSpec(const PdfSourceDocument& source, const PdfDictionary& fileSpec,
                      Attachment& attachment) {
    attachment.fileName = textOf(source, fileSpec, "UF");
    if (attachment.fileName.empty()) attachment.fileName = textOf(source, fileSpec, "F");
    attachment.description = textOf(source, fileSpec, "Desc");
    attachment.streamObject = embeddedStreamObject(source, fileSpec);

    if (attachment.streamObject <= 0) return;
    const PdfObject streamObject = source.object(attachment.streamObject);
    const PdfStream* stream = streamObject.asStream();
    if (stream == nullptr) return;

    if (const PdfObject* subtype = stream->dict.find("Subtype")) {
        const PdfObject resolved = source.resolve(*subtype);
        if (resolved.isName()) attachment.mimeType = resolved.asName();
    }
    if (const PdfObject* params = stream->dict.find("Params")) {
        const PdfObject resolved = source.resolve(*params);
        if (const PdfDictionary* dict = resolved.asDictionary()) {
            if (const PdfObject* size = dict->find("Size")) {
                const PdfObject resolvedSize = source.resolve(*size);
                if (resolvedSize.isNumber()) attachment.size = resolvedSize.asInteger();
            }
            attachment.creationDate = textOf(source, *dict, "CreationDate");
            attachment.modificationDate = textOf(source, *dict, "ModDate");
        }
    }
}

}  // namespace

std::vector<Attachment> listAttachments(const PdfSourceDocument& source) {
    std::vector<Attachment> result;

    const PdfObject* root = source.trailer().find("Root");
    if (root == nullptr || !root->isRef()) return result;
    const PdfObject catalog = source.object(root->asRef().number);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) return result;

    // 1. 文件層附件。
    if (const PdfObject* names = catalogDict->find("Names")) {
        const PdfObject resolvedNames = source.resolve(*names);
        if (const PdfDictionary* namesDict = resolvedNames.asDictionary()) {
            if (const PdfObject* embedded = namesDict->find("EmbeddedFiles")) {
                std::vector<std::pair<std::string, PdfObject>> raw;
                collectNameTree(source, *embedded, 0, raw);
                for (const auto& [name, value] : raw) {
                    const PdfObject resolved = source.resolve(value);
                    const PdfDictionary* fileSpec = resolved.asDictionary();
                    if (fileSpec == nullptr) continue;
                    Attachment attachment;
                    attachment.origin = AttachmentOrigin::DocumentLevel;
                    attachment.name = name;
                    fillFromFileSpec(source, *fileSpec, attachment);
                    result.push_back(std::move(attachment));
                }
            }
        }
    }

    // 2. 檔案附件註解。走頁面的 /Annots。
    const std::vector<PdfRef>& pages = source.pages();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PdfObject page = source.object(pages[i].number);
        const PdfDictionary* pageDict = page.asDictionary();
        if (pageDict == nullptr) continue;
        const PdfObject* annots = pageDict->find("Annots");
        if (annots == nullptr) continue;
        const PdfObject resolvedAnnots = source.resolve(*annots);
        const PdfArray* array = resolvedAnnots.asArray();
        if (array == nullptr) continue;

        for (const PdfObject& entry : *array) {
            const PdfObject annot = source.resolve(entry);
            const PdfDictionary* annotDict = annot.asDictionary();
            if (annotDict == nullptr) continue;
            const PdfObject* subtype = annotDict->find("Subtype");
            if (subtype == nullptr) continue;
            const PdfObject resolvedSubtype = source.resolve(*subtype);
            if (!resolvedSubtype.isName("FileAttachment")) continue;

            const PdfObject* fs = annotDict->find("FS");
            if (fs == nullptr) continue;
            const PdfObject resolvedFs = source.resolve(*fs);
            const PdfDictionary* fileSpec = resolvedFs.asDictionary();
            if (fileSpec == nullptr) continue;

            Attachment attachment;
            attachment.origin = AttachmentOrigin::FileAttachmentAnnot;
            attachment.pageIndex = static_cast<std::int32_t>(i);
            attachment.name = textOf(source, *annotDict, "T");
            fillFromFileSpec(source, *fileSpec, attachment);
            // 註解的 /Contents 優先於 filespec 的 /Desc：使用者在註解上打的字
            // 是他自己寫的說明，filespec 的 /Desc 多半是產生工具塞的。
            // 這一段必須在 fillFromFileSpec 之後，否則會被它寫的 /Desc 蓋掉。
            if (const std::string contents = textOf(source, *annotDict, "Contents");
                !contents.empty()) {
                attachment.description = contents;
            }
            result.push_back(std::move(attachment));
        }
    }

    // /Params /Size 與實際內容不一致時標示出來。照著 /Size 顯示「2 MB」
    // 卻只取得出 3 KB，使用者會以為是我們壞了。
    for (Attachment& attachment : result) {
        if (attachment.size < 0 || attachment.streamObject <= 0) continue;
        const ExtractResult extracted = extractAttachment(source, attachment);
        if (!extracted.ok) continue;
        attachment.sizeMismatch =
            static_cast<std::int64_t>(extracted.bytes.size()) != attachment.size;
    }

    return result;
}

ExtractResult extractAttachment(const PdfSourceDocument& source, const Attachment& attachment) {
    ExtractResult result;
    if (attachment.streamObject <= 0) {
        result.diagnostic = "附件沒有內容串流";
        return result;
    }
    const PdfObject object = source.object(attachment.streamObject);
    const PdfStream* stream = object.asStream();
    if (stream == nullptr) {
        result.diagnostic = "附件的內容物件不是串流";
        return result;
    }

    const objects::DecodeResult decoded = objects::decodeStream(
        *stream, [&source](const PdfRef& ref) { return source.object(ref.number); });
    if (!decoded.ok) {
        // 把沒解開的壓縮位元組寫成檔案，使用者拿到的是垃圾，而且他無從得知。
        result.diagnostic = decoded.diagnostic.empty() ? "不支援的串流濾鏡" : decoded.diagnostic;
        return result;
    }
    result.ok = true;
    result.bytes = decoded.data;
    return result;
}

std::string sanitizeAttachmentFileName(const std::string& raw) {
    // 附件檔名來自不可信輸入。可能是 "..\\..\\Windows\\System32\\x.dll"、
    // 可能是 "CON"、可能含控制字元。全部收斂掉，清不出東西就回空字串
    // 讓呼叫端要求使用者自己命名——猜一個名字給他反而更難察覺出了什麼事。
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) continue;             // 控制字元
        if (c == '/' || c == '\\') continue;              // 路徑分隔
        if (c == ':' || c == '*' || c == '?' || c == '"') continue;
        if (c == '<' || c == '>' || c == '|') continue;
        out.push_back(c);
    }
    // 前後空白與句點：Windows 會把結尾的句點與空白吃掉，"evil.exe." 存下來
    // 就變成 "evil.exe"。
    const auto notTrim = [](char c) { return c != ' ' && c != '.'; };
    const auto begin = std::find_if(out.begin(), out.end(), notTrim);
    const auto end = std::find_if(out.rbegin(), out.rend(), notTrim).base();
    out = (begin < end) ? std::string(begin, end) : std::string{};
    if (out.empty()) return {};

    // Windows 保留裝置名稱。用它們當檔名會開到裝置而不是檔案。
    static constexpr const char* kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2",
                                                "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                                "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                                "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string stem = out.substr(0, out.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const char* reserved : kReserved) {
        if (stem == reserved) return "_" + out;
    }
    return out;
}

}  // namespace alioth::engine::attachments
