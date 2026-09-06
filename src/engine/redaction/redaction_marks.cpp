#include "engine/redaction/redaction_marks.h"

#include <utility>

#include "domain/quad_point.h"
#include "engine/objects/page_object_editor.h"

namespace alioth::engine::redaction {
namespace {

using objects::IncrementalAppender;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;

std::string formatReal(double value) { return objects::formatReal(value); }

// 標記的外觀：只畫外框，不填色。
//
// 填成黑色會讓使用者以為已經塗黑完成，但這個階段底下的文字一個字都沒少。
// 「看起來已經處理好」是 Redaction 事故的直接成因之一，因此兩個階段的外觀
// 必須一眼可辨。
std::string markAppearanceContent(const domain::RedactionMark& mark) {
    std::string content;
    content += formatReal(mark.markColor.r) + ' ' + formatReal(mark.markColor.g) + ' ' +
               formatReal(mark.markColor.b) + " RG\n1 w\n";
    for (const domain::RectF& raw : mark.areas) {
        const domain::RectF area = raw.normalized();
        content += formatReal(area.left) + ' ' + formatReal(area.bottom) + ' ' +
                   formatReal(area.width()) + ' ' + formatReal(area.height()) + " re\n";
    }
    content += "S\n";
    return content;
}

PdfObject colorArray(const domain::ColorRgb& color) {
    return objects::makeNumberArray({color.r, color.g, color.b});
}

}  // namespace

MarkWriteResult writeRedactionMarks(IncrementalAppender& appender,
                                    const domain::RedactionMarkSet& marks) {
    MarkWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "增量附加器尚未開啟";
        return result;
    }

    for (const domain::RedactionMark& mark : marks.marks()) {
        if (!mark.isValid()) {
            result.diagnostic = "塗黑標記無效：頁碼為負或區域為空";
            return result;
        }
        objects::PdfRef page{};
        if (!objects::pageRefAt(appender, mark.pageIndex, page)) {
            result.diagnostic = "頁碼超出範圍：" + std::to_string(mark.pageIndex);
            return result;
        }

        const domain::RectF box = mark.boundingBox();

        PdfDictionary appearanceDict;
        appearanceDict.set("Type", objects::makeName("XObject"));
        appearanceDict.set("Subtype", objects::makeName("Form"));
        appearanceDict.set("FormType", PdfObject{static_cast<std::int64_t>(1)});
        appearanceDict.set("BBox",
                           objects::makeNumberArray({box.left, box.bottom, box.right, box.top}));
        appearanceDict.set("Matrix", objects::makeNumberArray({1, 0, 0, 1, 0, 0}));
        appearanceDict.set("Resources", PdfObject{PdfDictionary{}});

        const int appearanceObject = appender.allocateObject();
        appender.setObject(appearanceObject,
                           PdfObject{objects::PdfStream{std::move(appearanceDict),
                                                        markAppearanceContent(mark)}});

        PdfDictionary annotation;
        annotation.set("Type", objects::makeName("Annot"));
        annotation.set("Subtype", objects::makeName("Redact"));
        annotation.set("Rect",
                       objects::makeNumberArray({box.left, box.bottom, box.right, box.top}));

        std::vector<double> quads;
        for (const domain::RectF& area : mark.areas) {
            const std::vector<double> quad =
                domain::QuadPoint::fromRect(area.normalized()).toArray();
            quads.insert(quads.end(), quad.begin(), quad.end());
        }
        annotation.set("QuadPoints", objects::makeNumberArray(quads));

        annotation.set("C", colorArray(mark.markColor));
        annotation.set("IC", colorArray(mark.fillColor));
        // /F 4 = Print。標記本身要列印得出來，否則校閱者在紙本上看不到待處理的區域。
        annotation.set("F", PdfObject{static_cast<std::int64_t>(4)});
        if (!mark.author.empty()) annotation.set("T", objects::makeTextString(mark.author));
        if (!mark.subject.empty()) annotation.set("Subj", objects::makeTextString(mark.subject));
        if (!mark.note.empty()) annotation.set("Contents", objects::makeTextString(mark.note));
        if (mark.overlayText.has_value()) {
            annotation.set("OverlayText", objects::makeTextString(*mark.overlayText));
            annotation.set("Q", PdfObject{static_cast<std::int64_t>(0)});
            annotation.set("DA", objects::makeLiteralString(
                                     "0 g /Helv " + formatReal(mark.overlayFontSize) + " Tf"));
        }
        if (const std::string date = domain::toPdfDateString(mark.modified); !date.empty()) {
            annotation.set("M", objects::makeLiteralString(date));
        }

        PdfDictionary appearanceEntry;
        appearanceEntry.set("N", objects::makeRef(appearanceObject));
        annotation.set("AP", PdfObject{std::move(appearanceEntry)});

        const int annotationObject = appender.allocateObject();
        appender.setObject(annotationObject, PdfObject{std::move(annotation)});

        const objects::PageEditStatus status =
            objects::appendToPageArray(appender, page, "Annots", objects::makeRef(annotationObject));
        if (!status.ok) {
            result.diagnostic = status.diagnostic;
            return result;
        }
        result.annotationObjects.push_back(annotationObject);
    }

    result.ok = true;
    return result;
}

objects::BuildResult markRedactions(std::string sourceBytes, const domain::RedactionMarkSet& marks,
                                    std::string* diagnostic) {
    objects::BuildResult failed;
    IncrementalAppender appender;
    std::string openDiagnostic;
    const objects::SourceStatus status = appender.open(std::move(sourceBytes), &openDiagnostic);
    if (status != objects::SourceStatus::Ok) {
        failed.diagnostic = std::string{objects::describe(status)} + "：" + openDiagnostic;
        if (diagnostic != nullptr) *diagnostic = failed.diagnostic;
        return failed;
    }

    const MarkWriteResult written = writeRedactionMarks(appender, marks);
    if (!written.ok) {
        failed.diagnostic = written.diagnostic;
        if (diagnostic != nullptr) *diagnostic = failed.diagnostic;
        return failed;
    }
    return appender.build();
}

domain::RedactionMarkSet readRedactionMarks(const objects::PdfSourceDocument& source) {
    domain::RedactionMarkSet marks;
    const std::vector<objects::PdfRef>& pages = source.pages();
    for (std::size_t index = 0; index < pages.size(); ++index) {
        const PdfObject page = source.object(pages[index].number);
        const PdfDictionary* pageDict = page.asDictionary();
        if (pageDict == nullptr) continue;
        const PdfObject* annotsValue = pageDict->find("Annots");
        if (annotsValue == nullptr) continue;
        const PdfObject annots = source.resolve(*annotsValue);
        const PdfArray* array = annots.asArray();
        if (array == nullptr) continue;

        for (const PdfObject& entry : *array) {
            const PdfObject annotation = source.resolve(entry);
            const PdfDictionary* dict = annotation.asDictionary();
            if (dict == nullptr) continue;
            const PdfObject* subtype = dict->find("Subtype");
            if (subtype == nullptr || !subtype->isName("Redact")) continue;

            domain::RedactionMark mark;
            mark.pageIndex = static_cast<int>(index);

            // /QuadPoints 優先於 /Rect：跨行的標記在 /Rect 上會膨脹成一整塊，
            // 依 /Rect 套用會多刪掉行首行尾之間的內容。
            const PdfObject* quadsValue = dict->find("QuadPoints");
            const PdfObject quads =
                quadsValue == nullptr ? PdfObject{} : source.resolve(*quadsValue);
            if (const PdfArray* quadArray = quads.asArray();
                quadArray != nullptr && quadArray->size() >= 8) {
                for (std::size_t i = 0; i + 7 < quadArray->size(); i += 8) {
                    domain::QuadPoint quad{};
                    quad.upperLeft = {(*quadArray)[i].asNumber(), (*quadArray)[i + 1].asNumber()};
                    quad.upperRight = {(*quadArray)[i + 2].asNumber(),
                                       (*quadArray)[i + 3].asNumber()};
                    quad.lowerLeft = {(*quadArray)[i + 4].asNumber(),
                                      (*quadArray)[i + 5].asNumber()};
                    quad.lowerRight = {(*quadArray)[i + 6].asNumber(),
                                       (*quadArray)[i + 7].asNumber()};
                    mark.areas.push_back(quad.boundingBox());
                }
            } else if (const PdfObject* rectValue = dict->find("Rect")) {
                const PdfObject rect = source.resolve(*rectValue);
                if (const PdfArray* rectArray = rect.asArray();
                    rectArray != nullptr && rectArray->size() >= 4) {
                    mark.areas.push_back(domain::RectF{(*rectArray)[0].asNumber(),
                                                       (*rectArray)[1].asNumber(),
                                                       (*rectArray)[2].asNumber(),
                                                       (*rectArray)[3].asNumber()}
                                             .normalized());
                }
            }
            if (mark.areas.empty()) continue;

            if (const PdfObject* ic = dict->find("IC")) {
                const PdfObject color = source.resolve(*ic);
                if (const PdfArray* array3 = color.asArray();
                    array3 != nullptr && array3->size() >= 3) {
                    mark.fillColor = {(*array3)[0].asNumber(), (*array3)[1].asNumber(),
                                      (*array3)[2].asNumber()};
                }
            }
            marks.add(std::move(mark));
        }
    }
    return marks;
}

MarkWriteResult removeRedactionMarks(IncrementalAppender& appender, int pageIndex) {
    MarkWriteResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "增量附加器尚未開啟";
        return result;
    }

    const std::vector<objects::PdfRef>& pages = appender.source().pages();
    for (std::size_t index = 0; index < pages.size(); ++index) {
        if (pageIndex >= 0 && static_cast<int>(index) != pageIndex) continue;

        const PdfObject page = appender.currentObject(pages[index].number);
        const PdfDictionary* pageDict = page.asDictionary();
        if (pageDict == nullptr) continue;
        const PdfObject* annotsValue = pageDict->find("Annots");
        if (annotsValue == nullptr) continue;

        const bool indirect = annotsValue->isRef();
        const PdfObject annots =
            indirect ? appender.currentObject(annotsValue->asRef().number) : *annotsValue;
        const PdfArray* array = annots.asArray();
        if (array == nullptr) continue;

        PdfArray kept;
        bool changed = false;
        for (const PdfObject& entry : *array) {
            const PdfObject annotation =
                entry.isRef() ? appender.currentObject(entry.asRef().number) : entry;
            const PdfDictionary* dict = annotation.asDictionary();
            const PdfObject* subtype = dict == nullptr ? nullptr : dict->find("Subtype");
            if (subtype != nullptr && subtype->isName("Redact")) {
                changed = true;
                continue;
            }
            kept.push_back(entry);
        }
        if (!changed) continue;

        if (indirect) {
            if (!appender.updateObject(annotsValue->asRef().number, PdfObject{std::move(kept)})) {
                result.diagnostic = "無法更新 /Annots 陣列物件";
                return result;
            }
        } else {
            PdfDictionary updated = *pageDict;
            updated.set("Annots", PdfObject{std::move(kept)});
            if (!appender.updateObject(pages[index].number, PdfObject{std::move(updated)})) {
                result.diagnostic = "無法更新頁面字典";
                return result;
            }
        }
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::redaction
