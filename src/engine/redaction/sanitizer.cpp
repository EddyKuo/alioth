#include "engine/redaction/sanitizer.h"

#include <iterator>
#include <utility>

#include "engine/redaction/pdf_document_rewriter.h"

namespace alioth::engine::redaction {
namespace {

using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;

// /Info 裡會洩漏身分與環境的鍵。/CreationDate 與 /ModDate 也在內：
// 時間戳配上其他線索足以定位是誰在什麼機器上產生這份檔案。
constexpr const char* kInfoKeys[] = {"Title",   "Author",       "Subject", "Keywords",
                                     "Creator", "Producer",     "CreationDate",
                                     "ModDate", "Trapped"};

int clearDictionaryKeys(PdfDictionary& dict, const char* const* keys, std::size_t count) {
    int removed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (dict.has(keys[i])) {
            dict.remove(keys[i]);
            ++removed;
        }
    }
    return removed;
}

// 從 /Names 樹裡整支移除一個類別。逐筆刪除葉節點需要走完整棵名稱樹，
// 而整支移除的語意就是「這個類別不存在」，兩者對外行為相同。
int removeNameTree(PdfDocumentRewriter& document, PdfDictionary& catalog, const std::string& key) {
    PdfObject* namesValue = catalog.find("Names");
    if (namesValue == nullptr) return 0;

    PdfDictionary* names = nullptr;
    if (namesValue->isRef()) {
        PdfObject* target = document.object(namesValue->asRef().number);
        names = target == nullptr ? nullptr : target->asDictionary();
    } else {
        names = namesValue->asDictionary();
    }
    if (names == nullptr || !names->has(key)) return 0;
    names->remove(key);
    return 1;
}

}  // namespace

SanitizeResult sanitizeDocument(std::string sourceBytes, const SanitizeOptions& options) {
    SanitizeResult result;
    PdfDocumentRewriter document;
    std::string diagnostic;
    const objects::SourceStatus status = document.open(std::move(sourceBytes), &diagnostic);
    if (status != objects::SourceStatus::Ok) {
        result.diagnostic = std::string{objects::describe(status)};
        if (status == objects::SourceStatus::Encrypted) {
            result.diagnostic = "加密文件不支援中繼資料清除（ADR-002 驗收條件 5）";
        }
        if (!diagnostic.empty()) result.diagnostic += "：" + diagnostic;
        return result;
    }

    if (options.clearDocumentInfo) {
        const PdfObject* infoValue = document.trailer().find("Info");
        if (infoValue != nullptr) {
            if (infoValue->isRef()) {
                if (PdfObject* info = document.object(infoValue->asRef().number)) {
                    if (PdfDictionary* dict = info->asDictionary()) {
                        result.stats.clearedInfoKeys +=
                            clearDictionaryKeys(*dict, kInfoKeys, std::size(kInfoKeys));
                    }
                }
            }
            // /Info 清空後整個從 trailer 拿掉：留一個空字典仍然告訴讀者
            // 「這裡本來有東西」，而且部分工具會據此重新填入製作程式。
            document.trailer().remove("Info");
        }
    }

    objects::PdfRef catalogRef{};
    if (const PdfObject* root = document.trailer().find("Root"); root != nullptr && root->isRef()) {
        catalogRef = root->asRef();
    }
    PdfObject* catalogObject = document.object(catalogRef.number);
    PdfDictionary* catalog = catalogObject == nullptr ? nullptr : catalogObject->asDictionary();

    if (catalog != nullptr) {
        if (options.clearXmpMetadata && catalog->has("Metadata")) {
            catalog->remove("Metadata");
            ++result.stats.removedMetadataStreams;
        }
        if (options.clearPieceInfo && catalog->has("PieceInfo")) {
            catalog->remove("PieceInfo");
            ++result.stats.removedPieceInfo;
        }
        if (options.removeEmbeddedFiles) {
            result.stats.removedEmbeddedFiles += removeNameTree(document, *catalog, "EmbeddedFiles");
        }
        if (options.removeJavaScript) {
            result.stats.removedJavaScript += removeNameTree(document, *catalog, "JavaScript");
            // /OpenAction 與 /AA 是文件開啟時自動觸發的動作，最常見的內容
            // 就是 JavaScript。本產品不執行它，但別的檢視器會。
            if (catalog->has("OpenAction")) {
                catalog->remove("OpenAction");
                ++result.stats.removedJavaScript;
            }
            if (catalog->has("AA")) {
                catalog->remove("AA");
                ++result.stats.removedJavaScript;
            }
        }
    }

    for (int index = 0; index < document.pageCount(); ++index) {
        objects::PdfRef pageRef{};
        if (!document.pageRef(index, pageRef)) continue;
        PdfObject* pageObject = document.object(pageRef.number);
        PdfDictionary* page = pageObject == nullptr ? nullptr : pageObject->asDictionary();
        if (page == nullptr) continue;

        if (options.clearPieceInfo && page->has("PieceInfo")) {
            page->remove("PieceInfo");
            ++result.stats.removedPieceInfo;
        }
        if (options.clearXmpMetadata && page->has("Metadata")) {
            page->remove("Metadata");
            ++result.stats.removedMetadataStreams;
        }
        if (options.removeJavaScript && page->has("AA")) {
            page->remove("AA");
            ++result.stats.removedJavaScript;
        }
        if (options.removeEmbeddedFiles) {
            PdfArray* annots = unsharedDictionaryArray(document, pageRef.number, "Annots");
            if (annots == nullptr) continue;
            PdfArray kept;
            for (const PdfObject& entry : *annots) {
                const PdfObject annotation = document.resolve(entry);
                const PdfDictionary* dict = annotation.asDictionary();
                const PdfObject* subtype = dict == nullptr ? nullptr : dict->find("Subtype");
                if (subtype != nullptr && subtype->isName("FileAttachment")) {
                    ++result.stats.removedEmbeddedFiles;
                    continue;
                }
                kept.push_back(entry);
            }
            *annots = std::move(kept);
        }
    }

    result.bytes = document.build();
    result.ok = true;
    return result;
}

}  // namespace alioth::engine::redaction
