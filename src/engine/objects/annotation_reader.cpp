#include "engine/objects/annotation_reader.h"

#include <algorithm>
#include <string>

#include "engine/objects/page_object_editor.h"

namespace alioth::engine::objects {
namespace {

[[nodiscard]] std::optional<std::string> textOf(const PdfDictionary& dict, const char* key) {
    const PdfObject* value = dict.find(key);
    if (value == nullptr) return std::nullopt;
    const auto* text = std::get_if<PdfString>(&value->value());
    if (text == nullptr) return std::nullopt;

    // PDF 文字字串：非 ASCII 以 UTF-16BE 加 BOM 儲存（ISO 32000 §7.9.2.2）。
    // 領域層一律用 UTF-8，所以在這裡就轉回去——把編碼往上層送，每個呼叫端
    // 都得記得處理，而漏掉的那個會顯示成一串亂碼。
    const std::string& bytes = text->bytes;
    if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0xFE ||
        static_cast<unsigned char>(bytes[1]) != 0xFF) {
        return bytes;
    }
    std::string utf8;
    for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
        char32_t code = static_cast<char32_t>((static_cast<unsigned char>(bytes[i]) << 8) |
                                              static_cast<unsigned char>(bytes[i + 1]));
        // 代理對：BMP 以外的字（例如部分罕用漢字）佔兩個 UTF-16 碼元。
        // 不合併的話會輸出兩個無效碼點，落到檔案裡就是兩個問號。
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < bytes.size()) {
            const auto low = static_cast<char32_t>((static_cast<unsigned char>(bytes[i + 2]) << 8) |
                                                   static_cast<unsigned char>(bytes[i + 3]));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (code < 0x80) {
            utf8.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (code >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (code >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (code >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return utf8;
}

[[nodiscard]] std::vector<double> numbersOf(const PdfDictionary& dict, const char* key) {
    const PdfObject* value = dict.find(key);
    if (value == nullptr) return {};
    const PdfArray* array = value->asArray();
    if (array == nullptr) return {};
    std::vector<double> out;
    out.reserve(array->size());
    for (const PdfObject& item : *array) {
        // 一個非數字就整組作廢：拿半組座標畫出來的形狀比不畫更誤導。
        if (!item.isNumber()) return {};
        out.push_back(item.asNumber());
    }
    return out;
}

[[nodiscard]] std::optional<domain::ColorRgb> colorOf(const PdfDictionary& dict, const char* key) {
    const std::vector<double> parts = numbersOf(dict, key);
    // /C 在規格上也允許 0（透明）、1（灰階）、4（CMYK）個成分，但本產品全程用
    // RGB（見 domain::ColorRgb）。非 3 成分一律當成沒指定，用預設值而不是拿
    // 前三個數字硬湊——CMYK 的前三個數字湊出來的顏色是錯的。
    if (parts.size() != 3) return std::nullopt;
    return domain::ColorRgb{parts[0], parts[1], parts[2]};
}

[[nodiscard]] std::optional<domain::AnnotationType> typeFromSubtype(const std::string& subtype) {
    using domain::AnnotationType;
    if (subtype == "Highlight") return AnnotationType::Highlight;
    if (subtype == "Underline") return AnnotationType::Underline;
    if (subtype == "StrikeOut") return AnnotationType::StrikeOut;
    if (subtype == "Squiggly") return AnnotationType::Squiggly;
    if (subtype == "Square") return AnnotationType::Square;
    if (subtype == "Circle") return AnnotationType::Circle;
    if (subtype == "Line") return AnnotationType::Line;
    if (subtype == "Ink") return AnnotationType::Ink;
    if (subtype == "Text") return AnnotationType::Text;
    if (subtype == "Caret") return AnnotationType::Caret;
    if (subtype == "FreeText") return AnnotationType::FreeText;
    if (subtype == "Polygon") return AnnotationType::Polygon;
    if (subtype == "PolyLine") return AnnotationType::PolyLine;
    return std::nullopt;
}

[[nodiscard]] std::vector<domain::PointF> pointsFrom(const std::vector<double>& flat) {
    std::vector<domain::PointF> points;
    points.reserve(flat.size() / 2);
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
        points.push_back(domain::PointF{flat[i], flat[i + 1]});
    }
    return points;
}

void readGeometry(const PdfDictionary& dict, domain::AnnotationType type,
                  domain::Annotation& annotation) {
    using domain::AnnotationType;
    switch (type) {
        case AnnotationType::Highlight:
        case AnnotationType::Underline:
        case AnnotationType::StrikeOut:
        case AnnotationType::Squiggly: {
            domain::TextMarkupGeometry markup;
            markup.kind = type == AnnotationType::Highlight    ? domain::TextMarkupKind::Highlight
                          : type == AnnotationType::Underline  ? domain::TextMarkupKind::Underline
                          : type == AnnotationType::StrikeOut  ? domain::TextMarkupKind::StrikeOut
                                                               : domain::TextMarkupKind::Squiggly;
            const std::vector<double> flat = numbersOf(dict, "QuadPoints");
            for (std::size_t i = 0; i + 7 < flat.size(); i += 8) {
                markup.quads.push_back(domain::QuadPoint{{flat[i], flat[i + 1]},
                                                         {flat[i + 2], flat[i + 3]},
                                                         {flat[i + 4], flat[i + 5]},
                                                         {flat[i + 6], flat[i + 7]}});
            }
            annotation.geometry = std::move(markup);
            break;
        }
        case AnnotationType::Square:
            annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Square};
            break;
        case AnnotationType::Circle:
            annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Circle};
            break;
        case AnnotationType::Line: {
            domain::LineGeometry line;
            if (const std::vector<double> l = numbersOf(dict, "L"); l.size() >= 4) {
                line.start = domain::PointF{l[0], l[1]};
                line.end = domain::PointF{l[2], l[3]};
            }
            annotation.geometry = line;
            break;
        }
        case AnnotationType::Ink: {
            domain::InkGeometry ink;
            if (const PdfObject* value = dict.find("InkList"); value != nullptr) {
                if (const PdfArray* strokes = value->asArray()) {
                    for (const PdfObject& item : *strokes) {
                        const PdfArray* points = item.asArray();
                        if (points == nullptr) continue;
                        std::vector<double> flat;
                        flat.reserve(points->size());
                        bool ok = true;
                        for (const PdfObject& coordinate : *points) {
                            if (!coordinate.isNumber()) {
                                ok = false;
                                break;
                            }
                            flat.push_back(coordinate.asNumber());
                        }
                        if (!ok) continue;
                        std::vector<domain::PointF> stroke = pointsFrom(flat);
                        if (!stroke.empty()) ink.strokes.push_back(std::move(stroke));
                    }
                }
            }
            annotation.geometry = std::move(ink);
            break;
        }
        case AnnotationType::Text:
            annotation.geometry = domain::TextNoteGeometry{};
            break;
        case AnnotationType::Caret:
            annotation.geometry = domain::CaretGeometry{};
            break;
        case AnnotationType::FreeText: {
            domain::FreeTextGeometry freeText;
            freeText.text = annotation.contents;
            annotation.geometry = std::move(freeText);
            break;
        }
        case AnnotationType::Polygon: {
            domain::PolygonGeometry polygon;
            polygon.vertices = pointsFrom(numbersOf(dict, "Vertices"));
            annotation.geometry = std::move(polygon);
            break;
        }
        case AnnotationType::PolyLine: {
            domain::PolyLineGeometry polyline;
            polyline.vertices = pointsFrom(numbersOf(dict, "Vertices"));
            annotation.geometry = std::move(polyline);
            break;
        }
        default:
            break;
    }
}

}  // namespace

std::optional<domain::Annotation> readAnnotation(const PdfDictionary& dict) {
    std::string subtype;
    if (const PdfObject* value = dict.find("Subtype"); value != nullptr && value->isName()) {
        subtype = value->asName();
    }
    const std::optional<domain::AnnotationType> type = typeFromSubtype(subtype);
    if (!type.has_value()) return std::nullopt;

    domain::Annotation annotation;
    if (const auto id = textOf(dict, "NM")) annotation.id = *id;
    if (const auto author = textOf(dict, "T")) annotation.author = *author;
    if (const auto contents = textOf(dict, "Contents")) annotation.contents = *contents;
    if (const auto subject = textOf(dict, "Subj")) annotation.subject = *subject;
    if (const auto created = textOf(dict, "CreationDate")) {
        annotation.creationDate = domain::fromPdfDateString(*created);
    }
    if (const auto modified = textOf(dict, "M")) {
        annotation.modifiedDate = domain::fromPdfDateString(*modified);
    }
    if (const auto inReplyTo = textOf(dict, "IRT")) annotation.inReplyTo = *inReplyTo;
    if (const auto color = colorOf(dict, "C")) annotation.color = *color;
    annotation.interiorColor = colorOf(dict, "IC");
    if (const PdfObject* opacity = dict.find("CA"); opacity != nullptr && opacity->isNumber()) {
        annotation.opacity = std::clamp(opacity->asNumber(), 0.0, 1.0);
    }
    if (const PdfObject* flags = dict.find("F"); flags != nullptr && flags->isNumber()) {
        annotation.flags = static_cast<domain::AnnotationFlag>(flags->asInteger());
    }
    if (const PdfObject* border = dict.find("BS"); border != nullptr) {
        if (const PdfDictionary* bs = border->asDictionary()) {
            if (const PdfObject* width = bs->find("W"); width != nullptr && width->isNumber()) {
                annotation.border.width = width->asNumber();
            }
        }
    }
    if (const std::vector<double> rect = numbersOf(dict, "Rect"); rect.size() == 4) {
        annotation.rect = domain::RectF{rect[0], rect[1], rect[2], rect[3]}.normalized();
    }

    readGeometry(dict, *type, annotation);
    return annotation;
}

std::vector<PageAnnotation> readAllAnnotations(IncrementalAppender& appender) {
    std::vector<PageAnnotation> out;
    if (!appender.isOpen()) return out;

    for (int pageIndex = 0;; ++pageIndex) {
        PdfRef pageRef{};
        if (!pageRefAt(appender, pageIndex, pageRef)) break;
        const std::vector<int> annots = pageAnnotationRefs(appender.source(), pageRef);
        for (std::size_t i = 0; i < annots.size(); ++i) {
            const PdfObject object = appender.currentObject(annots[i]);
            const PdfDictionary* dict = object.asDictionary();
            if (dict == nullptr) continue;
            // 頁內序號用的是 /Annots 裡的位置，與註解列表和刪除路徑一致；
            // 跳過不支援的子型之後重新編號會讓兩邊對不上。
            if (std::optional<domain::Annotation> annotation = readAnnotation(*dict)) {
                out.push_back(PageAnnotation{pageIndex, static_cast<int>(i), std::move(*annotation)});
            }
        }
    }
    return out;
}

}  // namespace alioth::engine::objects
