#include "engine/objects/annotation_object_writer.h"

#include "engine/fonts/cjk_font_library.h"

#include "engine/fonts/cid_font_writer.h"

#include <algorithm>

#include <variant>

#include "engine/annotations/appearance_stream.h"
#include "engine/objects/page_object_editor.h"

namespace alioth::engine::objects {

namespace {

using annotations::Appearance;
using annotations::AppearanceOptions;
using annotations::BlendMode;
using annotations::ExtGState;
using domain::Annotation;
using domain::AnnotationType;
using domain::ColorRgb;
using domain::LineEnding;
using domain::RectF;

// /Popup 的預設尺寸。Acrobat 開新註釋視窗時用的也是這個量級；
// 尺寸只影響視窗初始位置，使用者拖動後會寫回自己的 /Rect。
constexpr double kPopupWidth = 180.0;
constexpr double kPopupHeight = 90.0;

[[nodiscard]] AnnotationWriteResult failure(std::string reason) {
    AnnotationWriteResult result{};
    result.diagnostic = std::move(reason);
    return result;
}

[[nodiscard]] PdfObject colorArray(const ColorRgb& color) {
    return makeNumberArray({color.r, color.g, color.b});
}

[[nodiscard]] PdfObject rectArray(const RectF& rect) {
    const RectF r = rect.normalized();
    return makeNumberArray({r.left, r.bottom, r.right, r.top});
}

[[nodiscard]] const char* lineEndingName(LineEnding ending) noexcept {
    switch (ending) {
        case LineEnding::None: return "None";
        case LineEnding::OpenArrow: return "OpenArrow";
        case LineEnding::ClosedArrow: return "ClosedArrow";
    }
    return "None";
}

[[nodiscard]] const char* borderStyleName(domain::BorderStyleKind kind) noexcept {
    switch (kind) {
        case domain::BorderStyleKind::Solid: return "S";
        case domain::BorderStyleKind::Dashed: return "D";
        case domain::BorderStyleKind::Beveled: return "B";
        case domain::BorderStyleKind::Inset: return "I";
        case domain::BorderStyleKind::Underline: return "U";
    }
    return "S";
}

// Type1／Helvetica 的字型字典。不內嵌字型程式——Helvetica 是 ISO 32000 附錄 D
// 的標準 14 之一，任何符合規格的檢視器都必須提供，因此直接寫內嵌（非間接）
// 字典即可，不需要另外配物件編號。與 engine/formbuild/form_field_writer.cpp
// 的 fontDictionary() 是同一份定義，這裡沒有連結該 target 的權利重用它
// （alioth_annotations 是 alioth_formbuild 的上游相依），因此各自持有一份；
// 兩份唯一允許不一致的地方是這個事實本身，內容必須保持相同。
[[nodiscard]] PdfObject helveticaFontDictionary() {
    PdfDictionary font;
    font.set("Type", makeName("Font"));
    font.set("Subtype", makeName("Type1"));
    font.set("BaseFont", makeName("Helvetica"));
    font.set("Encoding", makeName("WinAnsiEncoding"));
    return PdfObject{std::move(font)};
}

// /AP /N 的 /Resources。這正是 FPDFAnnot_SetAP 做不到、也是 ADR-002 存在的理由：
// 沒有這個字典，串流裡的 /GS0 就是懸空名稱，螢光筆的 Multiply 混合直接消失；
// FreeText 家族（WP24）少了這裡的 /Font /Helv，/Helv 同樣是懸空名稱，文字完全
// 不會被畫出來。
// 自訂圖章的影像 XObject。刻意不壓縮：附件與圖章的影像多半已經是壓縮格式，
// 而這裡拿到的是解碼後的像素，再壓一次要引入 zlib 的寫入路徑，
// 為了一張圖章不值得。/Length 由序列化器算，錯不了。
[[nodiscard]] PdfObject stampImageObject(const domain::StampGeometry& stamp, int smaskObject) {
    PdfDictionary dict;
    dict.set("Type", makeName("XObject"));
    dict.set("Subtype", makeName("Image"));
    dict.set("Width", PdfObject{static_cast<std::int64_t>(stamp.imageWidth)});
    dict.set("Height", PdfObject{static_cast<std::int64_t>(stamp.imageHeight)});
    dict.set("ColorSpace", makeName("DeviceRGB"));
    dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});
    if (smaskObject > 0) dict.set("SMask", makeRef(smaskObject, 0));

    std::string rgb;
    const std::size_t pixels = static_cast<std::size_t>(stamp.imageWidth) *
                               static_cast<std::size_t>(stamp.imageHeight);
    rgb.reserve(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t base = i * static_cast<std::size_t>(stamp.imageChannels);
        rgb.push_back(static_cast<char>(stamp.imagePixels[base]));
        rgb.push_back(static_cast<char>(stamp.imagePixels[base + 1]));
        rgb.push_back(static_cast<char>(stamp.imagePixels[base + 2]));
    }
    return PdfObject{PdfStream{std::move(dict), std::move(rgb)}};
}

// RGBA 的 alpha 抽成獨立的灰階 /SMask。PDF 的影像沒有交錯式 alpha，
// 把 RGBA 直接當 RGB 寫會讓顏色整片偏掉——每個像素多一個位元組，
// 之後所有像素都往後錯位一格。
[[nodiscard]] PdfObject stampSMaskObject(const domain::StampGeometry& stamp) {
    PdfDictionary dict;
    dict.set("Type", makeName("XObject"));
    dict.set("Subtype", makeName("Image"));
    dict.set("Width", PdfObject{static_cast<std::int64_t>(stamp.imageWidth)});
    dict.set("Height", PdfObject{static_cast<std::int64_t>(stamp.imageHeight)});
    dict.set("ColorSpace", makeName("DeviceGray"));
    dict.set("BitsPerComponent", PdfObject{static_cast<std::int64_t>(8)});

    std::string alpha;
    const std::size_t pixels = static_cast<std::size_t>(stamp.imageWidth) *
                               static_cast<std::size_t>(stamp.imageHeight);
    alpha.reserve(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        alpha.push_back(static_cast<char>(stamp.imagePixels[i * 4 + 3]));
    }
    return PdfObject{PdfStream{std::move(dict), std::move(alpha)}};
}

[[nodiscard]] PdfObject resourcesFor(const Appearance& appearance, int stampImageObject,
                                     int cjkFontObject) {
    PdfDictionary resources;
    if (!appearance.extGStates.empty()) {
        PdfDictionary states;
        for (const ExtGState& state : appearance.extGStates) {
            PdfDictionary entry;
            entry.set("Type", makeName("ExtGState"));
            // /CA 是描邊透明度、/ca 是填色透明度，只差大小寫，是最容易寫反的一對鍵。
            entry.set("CA", PdfObject{state.strokeAlpha});
            entry.set("ca", PdfObject{state.fillAlpha});
            entry.set("BM", makeName(state.blend == BlendMode::Multiply ? "Multiply" : "Normal"));
            states.set(state.name, PdfObject{std::move(entry)});
        }
        resources.set("ExtGState", PdfObject{std::move(states)});
    }
    if (appearance.needsFont || appearance.needsCjkFont) {
        PdfDictionary fonts;
        if (appearance.needsFont) fonts.set("Helv", helveticaFontDictionary());
        if (appearance.needsCjkFont && cjkFontObject > 0) {
            // 名稱要與 appearance_stream.cpp 寫進內容串流的 /CJK 一致。
            // 對不上的話中文**整段消失**——不是亂碼，是什麼都不畫，
            // 而且不會有任何錯誤訊息。
            fonts.set("CJK", makeRef(cjkFontObject, 0));
        }
        resources.set("Font", PdfObject{std::move(fonts)});
    }
    if (appearance.needsStampImage && stampImageObject > 0) {
        // 名稱要與 appearance_stream.cpp 寫進內容串流的 /Im0 一致。
        // 對不上的話圖章是空白的，而且不會有任何錯誤——那正是這條規則存在的理由。
        PdfDictionary xobjects;
        xobjects.set("Im0", makeRef(stampImageObject, 0));
        resources.set("XObject", PdfObject{std::move(xobjects)});
    }
    return PdfObject{std::move(resources)};
}

[[nodiscard]] PdfObject appearanceStreamObject(const Appearance& appearance,
                                               int stampImageObject, int cjkFontObject) {
    PdfDictionary dict;
    dict.set("Type", makeName("XObject"));
    dict.set("Subtype", makeName("Form"));
    dict.set("FormType", PdfObject{static_cast<std::int64_t>(1)});
    dict.set("BBox", rectArray(appearance.bbox));
    dict.set("Matrix", makeNumberArray({appearance.matrix[0], appearance.matrix[1],
                                        appearance.matrix[2], appearance.matrix[3],
                                        appearance.matrix[4], appearance.matrix[5]}));
    dict.set("Resources", resourcesFor(appearance, stampImageObject, cjkFontObject));
    return PdfObject{PdfStream{std::move(dict), appearance.content}};
}

void writeGeometryKeys(PdfDictionary& dict, const Annotation& annotation) {
    if (const auto* markup = std::get_if<domain::TextMarkupGeometry>(&annotation.geometry)) {
        PdfArray quads;
        quads.reserve(markup->quads.size() * 8);
        for (const domain::QuadPoint& q : markup->quads) {
            // 角序是 §12.5.6.10 的 (左上, 右上, 左下, 右下)。這個順序不直觀，
            // 寫成順時針或逆時針都會讓部分檢視器把標記畫成沙漏形。
            const double values[8] = {q.upperLeft.x,  q.upperLeft.y,  q.upperRight.x, q.upperRight.y,
                                      q.lowerLeft.x,  q.lowerLeft.y,  q.lowerRight.x, q.lowerRight.y};
            for (const double v : values) quads.emplace_back(v);
        }
        dict.set("QuadPoints", PdfObject{std::move(quads)});
        return;
    }
    if (const auto* ink = std::get_if<domain::InkGeometry>(&annotation.geometry)) {
        PdfArray strokes;
        for (const auto& stroke : ink->strokes) {
            if (stroke.empty()) continue;
            PdfArray points;
            points.reserve(stroke.size() * 2);
            for (const domain::PointF& p : stroke) {
                points.emplace_back(p.x);
                points.emplace_back(p.y);
            }
            strokes.emplace_back(std::move(points));
        }
        dict.set("InkList", PdfObject{std::move(strokes)});
        return;
    }
    if (const auto* line = std::get_if<domain::LineGeometry>(&annotation.geometry)) {
        dict.set("L", makeNumberArray({line->start.x, line->start.y, line->end.x, line->end.y}));
        PdfArray endings;
        endings.push_back(makeName(lineEndingName(line->startEnding)));
        endings.push_back(makeName(lineEndingName(line->endEnding)));
        dict.set("LE", PdfObject{std::move(endings)});
        return;
    }
    if (const auto* note = std::get_if<domain::TextNoteGeometry>(&annotation.geometry)) {
        dict.set("Open", PdfObject{note->open});
        dict.set("Name", makeName("Note"));
        return;
    }
    if (const auto* polygon = std::get_if<domain::PolygonGeometry>(&annotation.geometry)) {
        PdfArray vertices;
        vertices.reserve(polygon->vertices.size() * 2);
        for (const domain::PointF& p : polygon->vertices) {
            vertices.emplace_back(p.x);
            vertices.emplace_back(p.y);
        }
        dict.set("Vertices", PdfObject{std::move(vertices)});
        // /BE 邊框效果。雲線就是多邊形加上這個字典（ISO 32000-1 表 167）——
        // 少了它，外觀串流雖然畫成雲狀，但重新開啟後編輯器會把它當成一般
        // 多邊形，使用者一改就變回直邊。
        if (polygon->borderEffect.isCloudy()) {
            PdfDictionary effect;
            effect.set("S", makeName("C"));
            // /I 只定義 0/1/2，夾住而不是原樣寫出：超出範圍的值行為未定義。
            effect.set("I", PdfObject{std::clamp(polygon->borderEffect.intensity, 0.0, 2.0)});
            dict.set("BE", PdfObject{std::move(effect)});
        }
        return;
    }
    if (const auto* polyline = std::get_if<domain::PolyLineGeometry>(&annotation.geometry)) {
        PdfArray vertices;
        vertices.reserve(polyline->vertices.size() * 2);
        for (const domain::PointF& p : polyline->vertices) {
            vertices.emplace_back(p.x);
            vertices.emplace_back(p.y);
        }
        dict.set("Vertices", PdfObject{std::move(vertices)});
        PdfArray endings;
        endings.push_back(makeName(lineEndingName(polyline->startEnding)));
        endings.push_back(makeName(lineEndingName(polyline->endEnding)));
        dict.set("LE", PdfObject{std::move(endings)});
        return;
    }
    if (const auto* caret = std::get_if<domain::CaretGeometry>(&annotation.geometry)) {
        // /Sy 的合法值只有 "P"（段落符號）與 "None"；省略時 Acrobat 視同 "None"，
        // 但這裡一律明寫，讓讀回來的人不用去查規格預設值。
        dict.set("Sy", makeName(caret->symbol == domain::CaretSymbol::Paragraph ? "P" : "None"));
        return;
    }
    if (const auto* stamp = std::get_if<domain::StampGeometry>(&annotation.geometry)) {
        // 標準圖章寫 /Name，自訂圖章不寫：/Name 的值域是規格定死的，
        // 塞自訂字串進去會產出別人讀不懂的檔案，而且沒有任何好處——
        // 外觀已經在 /AP 裡了。
        if (const char* name = domain::stampNameOf(stamp->kind); name != nullptr) {
            dict.set("Name", makeName(name));
        }
        return;
    }
    if (const auto* freeText = std::get_if<domain::FreeTextGeometry>(&annotation.geometry)) {
        // /DA 是 FreeText 的必要鍵（ISO 32000-2 表 174）：Acrobat 用它決定使用者
        // 之後手動編輯這則註解時的預設字型與顏色，即使我們自己已經產生了 /AP。
        // 顏色分量在此攤平成 "r g b rg"——/DA 是單一字串而非結構化資料。
        std::string da = "/Helv " + annotations::formatNumber(freeText->fontSize) + " Tf " +
                         annotations::formatNumber(freeText->textColor.r) + " " +
                         annotations::formatNumber(freeText->textColor.g) + " " +
                         annotations::formatNumber(freeText->textColor.b) + " rg";
        dict.set("DA", makeLiteralString(da));
        dict.set("Q", PdfObject{static_cast<std::int64_t>(freeText->align == domain::TextAlign::Center
                                                               ? 1
                                                           : freeText->align == domain::TextAlign::Right
                                                               ? 2
                                                               : 0)});
        dict.set("IT", makeName(domain::freeTextIntentName(freeText->intent)));
        if (freeText->intent == domain::FreeTextIntent::Callout && freeText->callout.has_value()) {
            const domain::CalloutLine& line = *freeText->callout;
            std::vector<double> cl = {line.start.x, line.start.y};
            if (line.knee.has_value()) {
                cl.push_back(line.knee->x);
                cl.push_back(line.knee->y);
            }
            cl.push_back(line.end.x);
            cl.push_back(line.end.y);
            dict.set("CL", makeNumberArray(cl));
            // 與 /Line 的 /LE 不同：FreeText 的 /LE 只有一個名稱，套用在 /CL 的
            // 終點（指向目標的那一端），/CL 的起點（貼著文字框那一端）沒有端點樣式
            // 可設（ISO 32000-2 表 174）。
            dict.set("LE", makeName(lineEndingName(line.ending)));
        }
        return;
    }
}

// /Measure（ISO 32000-1 §12.5.6.11 表 261/262）。只有已校正比例的量測註解
// （Line 的距離、Polygon 的面積、PolyLine 的周長）才會帶這個鍵——沒有它，
// Acrobat 的「Analyze/Measuring」面板就不會把這則註解當成量測物件，
// 也是本檔判斷「是否要寫 /Measure」的唯一依據。
[[nodiscard]] PdfObject numberFormatDictionary(const std::string& unit, double factor) {
    PdfDictionary format;
    format.set("Type", makeName("NumberFormat"));
    format.set("U", makeName(unit));
    format.set("C", PdfObject{factor});
    return PdfObject{std::move(format)};
}

[[nodiscard]] PdfObject measureDictionary(const domain::MeasureInfo& measure, bool includeArea) {
    PdfDictionary dict;
    dict.set("Type", makeName("Measure"));
    dict.set("Subtype", makeName("RL"));
    dict.set("R", makeLiteralString(measure.ratioLabel));

    PdfArray x;
    x.push_back(numberFormatDictionary(measure.unitLabel, measure.unitsPerPoint));
    dict.set("X", PdfObject{std::move(x)});

    PdfArray d;
    d.push_back(numberFormatDictionary(measure.unitLabel, measure.unitsPerPoint));
    dict.set("D", PdfObject{std::move(d)});

    if (includeArea) {
        // /A 的 /C 是「點的平方」到「顯示單位的平方」的換算因子，
        // 因此是距離換算因子的平方，不是同一個數字。
        PdfArray a;
        a.push_back(numberFormatDictionary(measure.unitLabel + "2",
                                           measure.unitsPerPoint * measure.unitsPerPoint));
        dict.set("A", PdfObject{std::move(a)});
    }
    return PdfObject{std::move(dict)};
}

void writeBorderStyle(PdfDictionary& dict, const Annotation& annotation) {
    PdfDictionary bs;
    bs.set("Type", makeName("Border"));
    bs.set("W", PdfObject{annotation.border.width});
    bs.set("S", makeName(borderStyleName(annotation.border.style)));
    if (annotation.border.style == domain::BorderStyleKind::Dashed) {
        // /D 只在 /S 為 D 時有意義。空的虛線陣列會讓部分檢視器畫成完全不可見，
        // 因此沒給樣式時退回一個合理的預設而不是寫出空陣列。
        std::vector<double> pattern = annotation.border.dashPattern;
        if (pattern.empty()) pattern = {3.0};
        bs.set("D", makeNumberArray(pattern));
    }
    dict.set("BS", PdfObject{std::move(bs)});
}

}  // namespace

AnnotationWriteResult writeAnnotation(IncrementalAppender& appender, int pageIndex,
                                      const Annotation& annotation,
                                      const AnnotationWriteOptions& options) {
    if (!appender.isOpen()) return failure("附加器尚未開啟原檔");

    PdfRef pageRef{};
    if (!pageRefAt(appender, pageIndex, pageRef)) {
        return failure("頁碼超出範圍：" + std::to_string(pageIndex));
    }

    // 這條通道的存在意義就是能寫 /Resources，因此一律以「支援資源」模式產生外觀。
    AppearanceOptions appearanceOptions{};
    appearanceOptions.pageRotation = options.pageRotation;
    appearanceOptions.resourcesSupported = true;

    const Appearance appearance = annotations::generateAppearance(annotation, appearanceOptions);
    if (!appearance.valid) return failure("外觀串流產生失敗：" + appearance.diagnostic);

    // 就地改寫（屬性面板）與新增走同一條路，差別只在物件編號從哪裡來，
    // 以及要不要掛上 /Annots。兩份幾乎一樣的程式碼必然會分岔，而分岔的
    // 那一半寫出來的 /AP 會與另一半不同。
    const bool replacing = options.replaceObject.has_value();
    const int annotationNumber = replacing ? *options.replaceObject : appender.allocateObject();
    const int appearanceNumber = appender.allocateObject();
    // 回覆自己不帶 /Popup：Acrobat 以父註解的視窗顯示整條串，
    // 各自帶一個會讓回覆看起來像獨立註解，PRD-ANN-007 的驗收就過不了。
    //
    // Line／Polygon／PolyLine 沿用既有的 Line 排除慣例：這三型是量測家族
    // （PRD-ANN-014），量測結果以 /Contents 的標籤文字呈現，不需要另一個
    // 彈出視窗重複顯示同一個數字。
    const AnnotationType writeType = annotation.type();
    const bool isMeasurementFamily = writeType == AnnotationType::Line ||
                                     writeType == AnnotationType::Polygon ||
                                     writeType == AnnotationType::PolyLine;
    const bool wantPopup = options.createPopup && !options.inReplyToObject.has_value() &&
                           !isMeasurementFamily;
    // 就地改寫時沿用原本那顆 /Popup，不新建：新建一顆而舊的仍掛在 /Annots 上，
    // 使用者會看到同一則註解有兩個彈出視窗。
    const int popupNumber = replacing ? options.reusePopupObject.value_or(0)
                           : wantPopup ? appender.allocateObject()
                                       : 0;

    // 自訂圖片圖章的影像必須是獨立物件：/Resources /XObject 的值只能是參照，
    // 影像串流沒辦法內嵌在字典裡。
    int stampImageNumber = 0;
    if (appearance.needsStampImage) {
        const auto* stamp = std::get_if<domain::StampGeometry>(&annotation.geometry);
        if (stamp == nullptr || !stamp->hasImage()) {
            return failure("自訂圖章缺少影像資料");
        }
        int smaskNumber = 0;
        if (stamp->imageChannels == 4) {
            smaskNumber = appender.allocateObject();
            appender.setObject(smaskNumber, stampSMaskObject(*stamp));
        }
        stampImageNumber = appender.allocateObject();
        appender.setObject(stampImageNumber, stampImageObject(*stamp, smaskNumber));
    }

    // 內嵌 CJK 子集字型（ADR-007）。只有真的用到中文的註解才會做——
    // 純英文的註解不該把字型子集拖進檔案裡。
    int cjkFontNumber = 0;
    if (appearance.needsCjkFont) {
        auto& library = fonts::CjkFontLibrary::instance();
        const fonts::SubsetResult subset = library.subsetFor(appearance.cjkCodepoints);
        if (!subset.ok) {
            return failure("CJK 字型子集化失敗：" + subset.diagnostic);
        }
        const fonts::EmbeddedFontResult embedded =
            fonts::embedSubsetFont(appender, subset, library.baseName());
        if (!embedded.ok) {
            return failure("內嵌 CJK 字型失敗：" + embedded.diagnostic);
        }
        cjkFontNumber = embedded.fontObject;
    }

    appender.setObject(appearanceNumber,
                       appearanceStreamObject(appearance, stampImageNumber, cjkFontNumber));

    PdfDictionary annot;
    annot.set("Type", makeName("Annot"));
    annot.set("Subtype", makeName(domain::subtypeName(annotation.type())));
    // /Rect 取外觀串流實際塗佈的範圍：線寬、箭頭與波浪振幅都會超出幾何本身，
    // /Rect 太小會讓 Acrobat 把筆跡裁掉。
    annot.set("Rect", rectArray(appearance.bbox));
    annot.set("P", makeRef(pageRef.number, pageRef.generation));
    annot.set("F", PdfObject{static_cast<std::int64_t>(annotation.flags)});
    annot.set("C", colorArray(annotation.color));
    if (annotation.interiorColor.has_value()) {
        annot.set("IC", colorArray(*annotation.interiorColor));
    }
    annot.set("CA", PdfObject{annotation.opacity});
    writeBorderStyle(annot, annotation);
    writeGeometryKeys(annot, annotation);

    if (!annotation.id.empty()) annot.set("NM", makeLiteralString(annotation.id));
    if (!annotation.author.empty()) annot.set("T", makeTextString(annotation.author));
    if (!annotation.contents.empty()) annot.set("Contents", makeTextString(annotation.contents));
    if (!annotation.subject.empty()) annot.set("Subj", makeTextString(annotation.subject));
    if (const std::string created = domain::toPdfDateString(annotation.creationDate);
        !created.empty()) {
        annot.set("CreationDate", makeLiteralString(created));
    }
    if (const std::string modified = domain::toPdfDateString(annotation.modifiedDate);
        !modified.empty()) {
        annot.set("M", makeLiteralString(modified));
    }

    if (options.inReplyToObject.has_value()) {
        annot.set("IRT", makeRef(*options.inReplyToObject));
        annot.set("RT", makeName("R"));
        // /StateModel 與 /State 只在回覆註解上才有意義(ISO 32000-1 §12.5.6.19):
        // 狀態是「對父註解的一個回覆動作」,不是父註解自身的屬性。
        if (options.stateModel.has_value() && options.state.has_value()) {
            annot.set("StateModel", makeTextString(*options.stateModel));
            annot.set("State", makeTextString(*options.state));
        }
    }
    if (popupNumber != 0) annot.set("Popup", makeRef(popupNumber));

    if (annotation.measure.has_value() && annotation.measure->isCalibrated()) {
        // 只有量測三型（Line 距離、Polygon 面積、PolyLine 周長）寫 /Measure；
        // 其餘型別帶了 measure 也不寫，避免 Acrobat 把螢光筆之類的東西
        // 誤判成量測物件。/A 只在 Polygon（面積）才有意義。
        const AnnotationType annotationType = annotation.type();
        const bool isMeasurable = annotationType == AnnotationType::Line ||
                                  annotationType == AnnotationType::Polygon ||
                                  annotationType == AnnotationType::PolyLine;
        if (isMeasurable) {
            annot.set("Measure", measureDictionary(*annotation.measure,
                                                    annotationType == AnnotationType::Polygon));
        }
    }

    PdfDictionary ap;
    ap.set("N", makeRef(appearanceNumber));
    annot.set("AP", PdfObject{std::move(ap)});

    // 改寫既有物件要走 updateObject；setObject 只接受 allocateObject() 給的編號。
    if (replacing) {
        if (!appender.updateObject(annotationNumber, PdfObject{std::move(annot)})) {
            return failure("要改寫的註解不存在於原檔");
        }
    } else {
        appender.setObject(annotationNumber, PdfObject{std::move(annot)});
    }

    if (popupNumber != 0 && !replacing) {
        const RectF bbox = appearance.bbox.normalized();
        PdfDictionary popup;
        popup.set("Type", makeName("Annot"));
        popup.set("Subtype", makeName("Popup"));
        popup.set("Rect", makeNumberArray({bbox.right, bbox.top - kPopupHeight,
                                           bbox.right + kPopupWidth, bbox.top}));
        popup.set("Parent", makeRef(annotationNumber));
        popup.set("Open", PdfObject{false});
        // /Popup 不該出現在列印輸出上，因此刻意不設 Print 旗標。
        popup.set("F", PdfObject{static_cast<std::int64_t>(0)});
        appender.setObject(popupNumber, PdfObject{std::move(popup)});
    }

    // 就地改寫的那一則已經在 /Annots 裡了，再掛一次會讓它出現兩次。
    if (!replacing) {
        const PageEditStatus annots =
            appendToPageArray(appender, pageRef, "Annots", makeRef(annotationNumber));
        if (!annots.ok) return failure("掛上 /Annots 失敗：" + annots.diagnostic);

        if (popupNumber != 0) {
            // /Popup 也必須是頁面的註解之一，否則 Acrobat 找不到它。
            const PageEditStatus popupStatus =
                appendToPageArray(appender, pageRef, "Annots", makeRef(popupNumber));
            if (!popupStatus.ok) return failure("掛上 /Popup 失敗：" + popupStatus.diagnostic);
        }
    }

    AnnotationWriteResult result{};
    result.ok = true;
    result.annotationObject = annotationNumber;
    result.appearanceObject = appearanceNumber;
    result.popupObject = popupNumber;
    return result;
}

}  // namespace alioth::engine::objects
