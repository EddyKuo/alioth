#include "engine/annotations/annotation_writer.h"
#include "engine/pdfium_lock.h"

#include <fpdf_annot.h>
#include <fpdfview.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace alioth::engine::annotations {

using domain::Annotation;
using domain::AnnotationType;
using domain::ColorRgb;
using domain::QuadPoint;

namespace {

[[nodiscard]] unsigned int toByte(double component) noexcept {
    const double scaled = component * 255.0 + 0.5;
    if (!(scaled > 0.0)) return 0u;
    if (scaled >= 255.0) return 255u;
    return static_cast<unsigned int>(scaled);
}

// PDFium 的字串介面一律吃 UTF-16LE。自己轉而不借 Qt，是因為引擎轉接層
// 不連結 Qt——領域層與引擎層要能在無 GUI 的環境下建置與測試。
[[nodiscard]] std::vector<unsigned short> toUtf16(const std::string& utf8) {
    std::vector<unsigned short> out;
    out.reserve(utf8.size() + 1);
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
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
            code = 0xFFFD;  // 非法前導位元組，以替換字元帶過而非中止整個寫入
        }
        if (i + extra >= utf8.size()) {
            code = 0xFFFD;
            extra = 0;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(utf8[i + k]);
            if ((cont & 0xC0) != 0x80) {
                code = 0xFFFD;
                extra = k - 1;
                break;
            }
            code = (code << 6) | (cont & 0x3Fu);
        }
        i += extra + 1;

        if (code >= 0x10000 && code <= 0x10FFFF) {
            code -= 0x10000;
            out.push_back(static_cast<unsigned short>(0xD800 + (code >> 10)));
            out.push_back(static_cast<unsigned short>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<unsigned short>(code));
        }
    }
    out.push_back(0);
    return out;
}

[[nodiscard]] FPDF_ANNOTATION_SUBTYPE toSubtype(AnnotationType type) noexcept {
    switch (type) {
        case AnnotationType::Highlight: return FPDF_ANNOT_HIGHLIGHT;
        case AnnotationType::Underline: return FPDF_ANNOT_UNDERLINE;
        case AnnotationType::StrikeOut: return FPDF_ANNOT_STRIKEOUT;
        case AnnotationType::Squiggly:  return FPDF_ANNOT_SQUIGGLY;
        case AnnotationType::Square:    return FPDF_ANNOT_SQUARE;
        case AnnotationType::Circle:    return FPDF_ANNOT_CIRCLE;
        case AnnotationType::Line:      return FPDF_ANNOT_LINE;
        case AnnotationType::Ink:       return FPDF_ANNOT_INK;
        case AnnotationType::Text:      return FPDF_ANNOT_TEXT;
    }
    return FPDF_ANNOT_SQUARE;
}

void setStringIfPresent(FPDF_ANNOTATION annot, const char* key, const std::string& value) {
    if (value.empty()) return;
    const std::vector<unsigned short> utf16 = toUtf16(value);
    FPDFAnnot_SetStringValue(annot, key, utf16.data());
}

void writeQuadPoints(FPDF_ANNOTATION annot, const std::vector<QuadPoint>& quads) {
    for (const QuadPoint& q : quads) {
        FS_QUADPOINTSF fs{};
        fs.x1 = static_cast<float>(q.upperLeft.x);
        fs.y1 = static_cast<float>(q.upperLeft.y);
        fs.x2 = static_cast<float>(q.upperRight.x);
        fs.y2 = static_cast<float>(q.upperRight.y);
        fs.x3 = static_cast<float>(q.lowerLeft.x);
        fs.y3 = static_cast<float>(q.lowerLeft.y);
        fs.x4 = static_cast<float>(q.lowerRight.x);
        fs.y4 = static_cast<float>(q.lowerRight.y);
        FPDFAnnot_AppendAttachmentPoints(annot, &fs);
    }
}

void writeInkList(FPDF_ANNOTATION annot, const domain::InkGeometry& ink) {
    for (const auto& stroke : ink.strokes) {
        if (stroke.empty()) continue;
        std::vector<FS_POINTF> points;
        points.reserve(stroke.size());
        for (const domain::PointF& p : stroke) {
            points.push_back(FS_POINTF{static_cast<float>(p.x), static_cast<float>(p.y)});
        }
        FPDFAnnot_AddInkStroke(annot, points.data(), points.size());
    }
}

}  // namespace

WriteResult writeAnnotation(PageHandle page, const Annotation& annotation,
                            const AppearanceOptions& options) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    WriteResult result{};
    if (page == nullptr) {
        result.diagnostic = "頁面把手是空的";
        return result;
    }

    // FreeText（Text Box／Typewriter／Callout，WP24）一定需要 /Resources /Font
    // /Helv 才能把文字畫出來；Caret 沒有對應的 PDFium 建立 API。這條路徑本來就
    // 是 ADR-002 要淘汰的舊通道（見 CLAUDE.md 的硬性限制 1），toSubtype() 也沒有
    // 這兩型的對應，硬跑下去會靜默退化成 /Square——與其讓呼叫端拿到一個
    // subtype 錯誤的註解，不如在這裡明確拒絕（IL-4）。
    const AnnotationType earlyType = annotation.type();
    if (earlyType == AnnotationType::FreeText || earlyType == AnnotationType::Caret) {
        result.diagnostic =
            std::string("此路徑（FPDFAnnot_SetAP）不支援 ") + domain::subtypeName(earlyType) +
            "：PDFium 建不出 /AP 的 /Resources，請改用 engine::objects::writeAnnotation（ADR-002 物件層通道）";
        return result;
    }

    // PDFium 無法為 /AP 串流附加 /Resources，因此產生器一律以「無資源」模式輸出。
    // 這個限制寫死在這裡而不是留給呼叫端，是為了不讓上層有機會產出引用了
    // 懸空 /GS0 的外觀串流——那種 PDF 在 Acrobat 開得起來，在別的檢視器不一定。
    AppearanceOptions effective = options;
    effective.resourcesSupported = false;

    const Appearance appearance = generateAppearance(annotation, effective);
    if (!appearance.valid) {
        result.diagnostic = "外觀串流產生失敗：" + appearance.diagnostic;
        return result;
    }

    auto* pdfPage = static_cast<FPDF_PAGE>(page);
    const FPDF_ANNOTATION_SUBTYPE subtype = toSubtype(annotation.type());
    if (!FPDFAnnot_IsSupportedSubtype(subtype)) {
        result.diagnostic = std::string("PDFium 不支援建立此註解型別：") +
                            domain::subtypeName(annotation.type());
        return result;
    }

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(pdfPage, subtype);
    if (annot == nullptr) {
        result.diagnostic = "FPDFPage_CreateAnnot 失敗";
        return result;
    }

    // 順序有意義：FPDFAnnot_SetColor 與 FPDFAnnot_SetBorder 在註解已有外觀串流時
    // 會失敗或反過來把外觀串流刪掉，所以它們必須全部排在 SetAP 之前。
    const unsigned int alpha = toByte(annotation.opacity);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, toByte(annotation.color.r),
                       toByte(annotation.color.g), toByte(annotation.color.b), alpha);
    if (annotation.interiorColor.has_value()) {
        const ColorRgb& ic = *annotation.interiorColor;
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, toByte(ic.r), toByte(ic.g),
                           toByte(ic.b), alpha);
    }
    if (annotation.border.width > 0.0) {
        FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, static_cast<float>(annotation.border.width));
    }

    if (const auto* markup = std::get_if<domain::TextMarkupGeometry>(&annotation.geometry)) {
        writeQuadPoints(annot, markup->quads);
    } else if (const auto* ink = std::get_if<domain::InkGeometry>(&annotation.geometry)) {
        writeInkList(annot, *ink);
    }

    // /Rect 取外觀串流算出的實際塗佈範圍，而不是呼叫端給的框：線寬、箭頭與
    // 波浪振幅都會超出幾何本身，/Rect 太小會讓 Acrobat 把筆跡裁掉。
    FS_RECTF rect{};
    rect.left = static_cast<float>(appearance.bbox.left);
    rect.bottom = static_cast<float>(appearance.bbox.bottom);
    rect.right = static_cast<float>(appearance.bbox.right);
    rect.top = static_cast<float>(appearance.bbox.top);
    FPDFAnnot_SetRect(annot, &rect);

    setStringIfPresent(annot, "NM", annotation.id);
    setStringIfPresent(annot, "T", annotation.author);
    setStringIfPresent(annot, "Contents", annotation.contents);
    setStringIfPresent(annot, "Subj", annotation.subject);
    setStringIfPresent(annot, "CreationDate", domain::toPdfDateString(annotation.creationDate));
    setStringIfPresent(annot, "M", domain::toPdfDateString(annotation.modifiedDate));

    FPDFAnnot_SetFlags(annot, static_cast<int>(annotation.flags));

    // 內容串流刻意維持 7-bit ASCII：FPDFAnnot_SetAP 收 UTF-16LE 字串，PDFium
    // 內部再以 PDF 文字編碼寫回位元組；非 ASCII 會被編成 UTF-16BE 加 BOM，
    // 那對內容串流而言就是一份壞掉的串流。
    const std::vector<unsigned short> ap = toUtf16(appearance.content);
    result.appearanceWritten =
        FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, ap.data()) != 0;
    result.blendModeElided = appearance.resourcesElided;

    result.index = FPDFPage_GetAnnotIndex(pdfPage, annot);
    FPDFPage_CloseAnnot(annot);

    result.ok = result.appearanceWritten && result.index >= 0;
    if (!result.appearanceWritten) result.diagnostic = "FPDFAnnot_SetAP 失敗";
    return result;
}

}  // namespace alioth::engine::annotations
