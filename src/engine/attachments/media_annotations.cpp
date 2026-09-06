#include "engine/attachments/media_annotations.h"

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

std::string textOf(const PdfSourceDocument& source, const PdfDictionary& dict, const char* key) {
    const PdfObject* value = dict.find(key);
    if (value == nullptr) return {};
    const PdfObject resolved = source.resolve(*value);
    if (const PdfString* text = std::get_if<PdfString>(&resolved.value())) return text->bytes;
    if (resolved.isName()) return resolved.asName();
    return {};
}

domain::RectF rectOf(const PdfSourceDocument& source, const PdfDictionary& dict) {
    const PdfObject* value = dict.find("Rect");
    if (value == nullptr) return {};
    const PdfObject resolved = source.resolve(*value);
    const PdfArray* array = resolved.asArray();
    if (array == nullptr || array->size() < 4) return {};
    const double a = source.resolve((*array)[0]).asNumber();
    const double b = source.resolve((*array)[1]).asNumber();
    const double c = source.resolve((*array)[2]).asNumber();
    const double d = source.resolve((*array)[3]).asNumber();
    return domain::RectF{a, b, c, d}.normalized();
}

// 從 filespec 取出內嵌串流的物件編號。與附件那條路一樣的形狀，
// 但刻意不共用同一個函式：媒體的 filespec 常常沒有 /EF（只有外部參照），
// 而附件那邊把「沒有 /EF」當成無效項目。兩邊的正確行為不同。
int embeddedStreamOf(const PdfSourceDocument& source, const PdfDictionary& fileSpec) {
    const PdfObject* ef = fileSpec.find("EF");
    if (ef == nullptr) return 0;
    const PdfObject resolved = source.resolve(*ef);
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) return 0;
    for (const char* key : {"F", "UF", "DOS", "Mac", "Unix"}) {
        if (const PdfObject* entry = dict->find(key); entry != nullptr && entry->isRef()) {
            return entry->asRef().number;
        }
    }
    return 0;
}

// /Screen 的媒體在 /A /R /C /D（MediaClip）底下，路徑很深。走不到底時
// 回傳 false 而不是猜測——猜錯會讓面板顯示一個取不出東西的可另存項目。
bool fillFromMediaClip(const PdfSourceDocument& source, const PdfDictionary& annot,
                       MediaAnnotation& media) {
    const PdfObject* action = annot.find("A");
    if (action == nullptr) return false;
    const PdfObject resolvedAction = source.resolve(*action);
    const PdfDictionary* actionDict = resolvedAction.asDictionary();
    if (actionDict == nullptr) return false;

    const PdfObject* rendition = actionDict->find("R");
    if (rendition == nullptr) return false;
    const PdfObject resolvedRendition = source.resolve(*rendition);
    const PdfDictionary* renditionDict = resolvedRendition.asDictionary();
    if (renditionDict == nullptr) return false;

    const PdfObject* clipContainer = renditionDict->find("C");
    if (clipContainer == nullptr) return false;
    const PdfObject resolvedClip = source.resolve(*clipContainer);
    const PdfDictionary* clipDict = resolvedClip.asDictionary();
    if (clipDict == nullptr) return false;

    media.mimeType = textOf(source, *clipDict, "CT");

    const PdfObject* data = clipDict->find("D");
    if (data == nullptr) return false;
    const PdfObject resolvedData = source.resolve(*data);
    const PdfDictionary* dataDict = resolvedData.asDictionary();
    if (dataDict == nullptr) return false;

    media.fileName = textOf(source, *dataDict, "UF");
    if (media.fileName.empty()) media.fileName = textOf(source, *dataDict, "F");
    media.streamObject = embeddedStreamOf(source, *dataDict);
    if (media.streamObject == 0) {
        // 只有外部參照。/F 在這裡是路徑或網址，不是內嵌資料。
        media.externalReference = media.fileName;
        media.fileName.clear();
    }
    return true;
}

// /RichMedia 的資產在 /RichMediaContent /Assets 名稱樹。只取第一個資產：
// 一則 RichMedia 可以夾帶整包資源（影片加字型加腳本），面板要的是
// 「這裡有一段媒體」而不是把整包攤開。
void fillFromRichMedia(const PdfSourceDocument& source, const PdfDictionary& annot,
                       MediaAnnotation& media) {
    const PdfObject* content = annot.find("RichMediaContent");
    if (content == nullptr) return;
    const PdfObject resolvedContent = source.resolve(*content);
    const PdfDictionary* contentDict = resolvedContent.asDictionary();
    if (contentDict == nullptr) return;

    const PdfObject* assets = contentDict->find("Assets");
    if (assets == nullptr) return;
    const PdfObject resolvedAssets = source.resolve(*assets);
    const PdfDictionary* assetsDict = resolvedAssets.asDictionary();
    if (assetsDict == nullptr) return;

    const PdfObject* names = assetsDict->find("Names");
    if (names == nullptr) return;
    const PdfObject resolvedNames = source.resolve(*names);
    const PdfArray* array = resolvedNames.asArray();
    if (array == nullptr || array->size() < 2) return;

    const PdfObject key = source.resolve((*array)[0]);
    if (const PdfString* text = std::get_if<PdfString>(&key.value())) media.fileName = text->bytes;

    const PdfObject fileSpec = source.resolve((*array)[1]);
    if (const PdfDictionary* dict = fileSpec.asDictionary(); dict != nullptr) {
        media.streamObject = embeddedStreamOf(source, *dict);
    }
}

// /Movie 是 PDF 1.2 的舊形態：/Movie /F 直接是 filespec。
void fillFromMovie(const PdfSourceDocument& source, const PdfDictionary& annot,
                   MediaAnnotation& media) {
    const PdfObject* movie = annot.find("Movie");
    if (movie == nullptr) return;
    const PdfObject resolved = source.resolve(*movie);
    const PdfDictionary* movieDict = resolved.asDictionary();
    if (movieDict == nullptr) return;

    const PdfObject* file = movieDict->find("F");
    if (file == nullptr) return;
    const PdfObject resolvedFile = source.resolve(*file);
    if (const PdfString* text = std::get_if<PdfString>(&resolvedFile.value())) {
        // 舊形態的 /F 幾乎一定是外部路徑：PDF 1.2 沒有內嵌檔案的機制。
        media.externalReference = text->bytes;
        return;
    }
    if (const PdfDictionary* dict = resolvedFile.asDictionary(); dict != nullptr) {
        media.fileName = textOf(source, *dict, "F");
        media.streamObject = embeddedStreamOf(source, *dict);
        if (media.streamObject == 0) {
            media.externalReference = media.fileName;
            media.fileName.clear();
        }
    }
}

}  // namespace

std::vector<MediaAnnotation> listMediaAnnotations(const PdfSourceDocument& source) {
    std::vector<MediaAnnotation> result;

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
            const PdfDictionary* dict = annot.asDictionary();
            if (dict == nullptr) continue;
            const PdfObject* subtype = dict->find("Subtype");
            if (subtype == nullptr) continue;
            const PdfObject resolvedSubtype = source.resolve(*subtype);

            MediaAnnotation media;
            if (resolvedSubtype.isName("Screen")) {
                media.kind = MediaKind::Screen;
            } else if (resolvedSubtype.isName("Movie")) {
                media.kind = MediaKind::Movie;
            } else if (resolvedSubtype.isName("RichMedia")) {
                media.kind = MediaKind::RichMedia;
            } else {
                continue;
            }

            media.pageIndex = static_cast<std::int32_t>(i);
            media.rectPt = rectOf(source, *dict);
            media.title = textOf(source, *dict, "T");
            media.description = textOf(source, *dict, "Contents");

            switch (media.kind) {
                case MediaKind::Screen:    fillFromMediaClip(source, *dict, media); break;
                case MediaKind::RichMedia: fillFromRichMedia(source, *dict, media); break;
                case MediaKind::Movie:     fillFromMovie(source, *dict, media); break;
            }

            result.push_back(std::move(media));
        }
    }
    return result;
}

MediaExtractResult extractMedia(const PdfSourceDocument& source, const MediaAnnotation& media) {
    MediaExtractResult result;
    if (!media.hasEmbeddedData()) {
        result.diagnostic = "這則媒體註解沒有內嵌資料，只有外部參照";
        return result;
    }
    const PdfObject object = source.object(media.streamObject);
    const PdfStream* stream = object.asStream();
    if (stream == nullptr) {
        result.diagnostic = "媒體的內容物件不是串流";
        return result;
    }

    const objects::DecodeResult decoded = objects::decodeStream(
        *stream, [&source](const PdfRef& ref) { return source.object(ref.number); });
    if (!decoded.ok) {
        // 把沒解開的壓縮位元組存成 .mp4 交給使用者，他只會得到一個播不開的
        // 檔案，而且不知道為什麼。
        result.diagnostic = decoded.diagnostic.empty() ? "不支援的串流濾鏡" : decoded.diagnostic;
        return result;
    }
    result.ok = true;
    result.bytes = decoded.data;
    return result;
}

std::string mediaNotice(const MediaAnnotation& media) {
    // 面板上必須明講「本程式不會播放」，而不是放一個按了沒反應的播放鍵。
    if (media.hasEmbeddedData()) {
        return "本程式不會播放內嵌媒體。可另存為檔案後以您信任的播放器開啟。";
    }
    if (!media.externalReference.empty()) {
        return "這則註解指向外部媒體，本程式不會自動開啟它。";
    }
    return "這則媒體註解沒有可取出的內容。";
}

}  // namespace alioth::engine::attachments
