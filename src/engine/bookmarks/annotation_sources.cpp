#include "engine/bookmarks/annotation_sources.h"

#include <algorithm>
#include <variant>

#include "engine/bookmarks/destination_codec.h"

namespace alioth::engine::bookmarks {

using domain::bookmarks::HighlightSource;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::PdfString;

std::vector<HighlightRecord> collectHighlights(const PdfSourceDocument& source) {
    std::vector<HighlightRecord> records;

    const std::vector<PdfRef>& pages = source.pages();
    for (std::size_t index = 0; index < pages.size(); ++index) {
        const PdfObject page = source.object(pages[index].number);
        const PdfDictionary* pageDict = page.asDictionary();
        if (pageDict == nullptr) continue;
        const PdfObject* annotsEntry = pageDict->find("Annots");
        if (annotsEntry == nullptr) continue;
        const PdfObject annots = source.resolve(*annotsEntry);
        const PdfArray* array = annots.asArray();
        if (array == nullptr) continue;

        for (const PdfObject& entry : *array) {
            const PdfObject annot = source.resolve(entry);
            const PdfDictionary* dict = annot.asDictionary();
            if (dict == nullptr) continue;
            const PdfObject* subtype = dict->find("Subtype");
            if (subtype == nullptr || !source.resolve(*subtype).isName("Highlight")) continue;

            HighlightRecord record;
            record.source.pageIndex = static_cast<std::int32_t>(index);

            if (const PdfObject* rect = dict->find("Rect")) {
                const PdfObject resolved = source.resolve(*rect);
                if (const PdfArray* box = resolved.asArray()) {
                    if (box->size() >= 4) {
                        const double bottom = std::min((*box)[1].asNumber(), (*box)[3].asNumber());
                        const double top = std::max((*box)[1].asNumber(), (*box)[3].asNumber());
                        record.source.leftPt =
                            std::min((*box)[0].asNumber(), (*box)[2].asNumber());
                        record.source.topPt = top;
                        // /Rect 可能是反向寫的（右上在前）。不正規化的話，
                        // 同頁的排序會依一個時大時小的數字進行，順序看起來像隨機的。
                        (void)bottom;
                    }
                }
            }

            if (const PdfObject* contents = dict->find("Contents")) {
                const PdfObject resolved = source.resolve(*contents);
                if (const PdfString* string = std::get_if<PdfString>(&resolved.value())) {
                    record.source.text = decodeTextString(*string);
                }
            }
            record.needsTextLookup = record.source.text.empty();
            records.push_back(std::move(record));
        }
    }

    std::stable_sort(records.begin(), records.end(),
                     [](const HighlightRecord& a, const HighlightRecord& b) {
                         if (a.source.pageIndex != b.source.pageIndex) {
                             return a.source.pageIndex < b.source.pageIndex;
                         }
                         if (a.source.topPt != b.source.topPt) return a.source.topPt > b.source.topPt;
                         return a.source.leftPt < b.source.leftPt;
                     });
    return records;
}

std::vector<HighlightSource> highlightSources(const std::vector<HighlightRecord>& records,
                                              bool includeUntitled) {
    std::vector<HighlightSource> sources;
    sources.reserve(records.size());
    for (const HighlightRecord& record : records) {
        if (record.needsTextLookup && !includeUntitled) continue;
        sources.push_back(record.source);
    }
    return sources;
}

}  // namespace alioth::engine::bookmarks
