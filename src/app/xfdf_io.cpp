#include "app/xfdf_io.h"

#include <QByteArray>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>

namespace alioth::app {

namespace {

// 輸入大小上限:單純的巨大檔案就先耗盡記憶體，不需要靠著解析才發現。
// XFDF 是文字格式，10 MB 已經遠超過審閱意見的合理規模。
constexpr std::size_t kMaxInputBytes = 10 * 1024 * 1024;

// XXE / billion-laughs 的唯一防線:直接拒絕任何 DOCTYPE 宣告。
// 大小寫、前後空白都要抓到，因為攻擊者不會乖乖照最常見的寫法來。
bool containsDoctype(const std::string& xml) {
    std::string upper;
    upper.reserve(xml.size());
    for (const char c : xml) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper.find("<!DOCTYPE") != std::string::npos;
}

[[nodiscard]] std::string colorToHex(const domain::ColorRgb& color) {
    const auto channel = [](double v) {
        const int i = std::clamp(static_cast<int>(v * 255.0 + 0.5), 0, 255);
        static const char* kHex = "0123456789ABCDEF";
        std::string out(2, '0');
        out[0] = kHex[(i >> 4) & 0xF];
        out[1] = kHex[i & 0xF];
        return out;
    };
    return "#" + channel(color.r) + channel(color.g) + channel(color.b);
}

[[nodiscard]] std::optional<domain::ColorRgb> hexToColor(const QString& text) {
    QString hex = text;
    if (hex.startsWith('#')) hex.remove(0, 1);
    if (hex.size() != 6) return std::nullopt;
    bool ok = true;
    const int r = hex.mid(0, 2).toInt(&ok, 16);
    const int g = ok ? hex.mid(2, 2).toInt(&ok, 16) : 0;
    const int b = ok ? hex.mid(4, 2).toInt(&ok, 16) : 0;
    if (!ok) return std::nullopt;
    return domain::ColorRgb{r / 255.0, g / 255.0, b / 255.0};
}

[[nodiscard]] std::vector<double> parseNumberList(const QString& text) {
    std::vector<double> out;
    for (const QString& part : text.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const double v = part.trimmed().toDouble(&ok);
        if (ok) out.push_back(v);
    }
    return out;
}

// XFDF 的 rect="left,bottom,right,top"(與 /Rect 一致)。
[[nodiscard]] std::optional<domain::RectF> parseRect(const QString& text) {
    const std::vector<double> v = parseNumberList(text);
    if (v.size() != 4) return std::nullopt;
    return domain::RectF{v[0], v[1], v[2], v[3]};
}

// 解析規則本身住在 domain::fromPdfDateString——FDF 匯入需要同一套規則，
// 兩份實作遲早會在「時區怎麼處理」上分岔（IL-3）。
[[nodiscard]] domain::PdfDate parsePdfDate(const QString& text) {
    return domain::fromPdfDateString(text.toStdString());
}

[[nodiscard]] QString escapeAttribute(const std::string& s) {
    return QString::fromStdString(s);  // QXmlStreamWriter 自己會做屬性跳脫
}

void writeQuadPoints(QXmlStreamWriter& xml, const std::vector<domain::QuadPoint>& quads) {
    QString text;
    for (const domain::QuadPoint& q : quads) {
        if (!text.isEmpty()) text += ',';
        const std::array<double, 8> values = {q.upperLeft.x,  q.upperLeft.y,  q.upperRight.x,
                                              q.upperRight.y, q.lowerLeft.x,  q.lowerLeft.y,
                                              q.lowerRight.x, q.lowerRight.y};
        QString one;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) one += ',';
            one += QString::number(values[i], 'f', 4);
        }
        text += one;
    }
    xml.writeTextElement(QStringLiteral("quadpoints"), text);
}

[[nodiscard]] std::vector<domain::QuadPoint> parseQuadPoints(const QString& text) {
    const std::vector<double> v = parseNumberList(text);
    std::vector<domain::QuadPoint> quads;
    for (std::size_t i = 0; i + 8 <= v.size(); i += 8) {
        domain::QuadPoint q;
        q.upperLeft = {v[i + 0], v[i + 1]};
        q.upperRight = {v[i + 2], v[i + 3]};
        q.lowerLeft = {v[i + 4], v[i + 5]};
        q.lowerRight = {v[i + 6], v[i + 7]};
        quads.push_back(q);
    }
    return quads;
}

[[nodiscard]] const char* elementNameFor(domain::AnnotationType type) noexcept {
    switch (type) {
        case domain::AnnotationType::Highlight: return "highlight";
        case domain::AnnotationType::Underline: return "underline";
        case domain::AnnotationType::StrikeOut: return "strikeout";
        case domain::AnnotationType::Squiggly:  return "squiggly";
        case domain::AnnotationType::Square:    return "square";
        case domain::AnnotationType::Circle:    return "circle";
        case domain::AnnotationType::Line:      return "line";
        case domain::AnnotationType::Ink:       return "ink";
        case domain::AnnotationType::Text:      return "text";
        case domain::AnnotationType::Caret:     return "caret";
        case domain::AnnotationType::FreeText:  return "freetext";
        case domain::AnnotationType::Polygon:   return "polygon";
        case domain::AnnotationType::PolyLine:  return "polyline";
    }
    return "highlight";
}

void writeCommonAttributes(QXmlStreamWriter& xml, const domain::Annotation& annotation,
                           std::int32_t pageIndex) {
    xml.writeAttribute(QStringLiteral("page"), QString::number(pageIndex));
    xml.writeAttribute(QStringLiteral("color"), QString::fromStdString(colorToHex(annotation.color)));
    xml.writeAttribute(QStringLiteral("opacity"), QString::number(annotation.opacity, 'f', 4));
    if (!annotation.id.empty()) {
        xml.writeAttribute(QStringLiteral("name"), escapeAttribute(annotation.id));
    }
    if (!annotation.author.empty()) {
        xml.writeAttribute(QStringLiteral("title"), escapeAttribute(annotation.author));
    }
    if (!annotation.subject.empty()) {
        xml.writeAttribute(QStringLiteral("subject"), escapeAttribute(annotation.subject));
    }
    if (const std::string created = domain::toPdfDateString(annotation.creationDate);
        !created.empty()) {
        xml.writeAttribute(QStringLiteral("creationdate"), QString::fromStdString(created));
    }
    if (const std::string modified = domain::toPdfDateString(annotation.modifiedDate);
        !modified.empty()) {
        xml.writeAttribute(QStringLiteral("date"), QString::fromStdString(modified));
    }
    {
        // rect 一律寫出:即使是零矩形也讓匯入端讀到明確的值，
        // 好過讓它去猜「沒寫代表什麼」。
        const domain::RectF r = annotation.rect.normalized();
        xml.writeAttribute(QStringLiteral("rect"),
                           QStringLiteral("%1,%2,%3,%4")
                               .arg(r.left, 0, 'f', 4)
                               .arg(r.bottom, 0, 'f', 4)
                               .arg(r.right, 0, 'f', 4)
                               .arg(r.top, 0, 'f', 4));
    }
    if (annotation.inReplyTo.has_value()) {
        xml.writeAttribute(QStringLiteral("inreplyto"), escapeAttribute(*annotation.inReplyTo));
        xml.writeAttribute(QStringLiteral("replyType"), QStringLiteral("R"));
    }
}

void writeAnnotationElement(QXmlStreamWriter& xml, const XfdfEntry& entry) {
    const domain::Annotation& annotation = entry.annotation;
    const domain::AnnotationType type = annotation.type();
    xml.writeStartElement(QString::fromLatin1(elementNameFor(type)));
    writeCommonAttributes(xml, annotation, entry.pageIndex);

    if (const auto* shape = std::get_if<domain::ShapeGeometry>(&annotation.geometry)) {
        (void)shape;
        xml.writeAttribute(QStringLiteral("width"), QString::number(annotation.border.width, 'f', 2));
        if (annotation.interiorColor.has_value()) {
            xml.writeAttribute(QStringLiteral("interior-color"),
                               QString::fromStdString(colorToHex(*annotation.interiorColor)));
        }
    } else if (const auto* line = std::get_if<domain::LineGeometry>(&annotation.geometry)) {
        xml.writeAttribute(QStringLiteral("width"), QString::number(annotation.border.width, 'f', 2));
        xml.writeAttribute(QStringLiteral("line"),
                           QStringLiteral("%1,%2,%3,%4")
                               .arg(line->start.x, 0, 'f', 4)
                               .arg(line->start.y, 0, 'f', 4)
                               .arg(line->end.x, 0, 'f', 4)
                               .arg(line->end.y, 0, 'f', 4));
    } else if (const auto* note = std::get_if<domain::TextNoteGeometry>(&annotation.geometry)) {
        (void)note;
    } else if (const auto* freeText = std::get_if<domain::FreeTextGeometry>(&annotation.geometry)) {
        xml.writeAttribute(QStringLiteral("fontsize"), QString::number(freeText->fontSize, 'f', 2));
    }

    if (!annotation.contents.empty()) {
        xml.writeTextElement(QStringLiteral("contents"), QString::fromStdString(annotation.contents));
    }

    if (const auto* markup = std::get_if<domain::TextMarkupGeometry>(&annotation.geometry)) {
        writeQuadPoints(xml, markup->quads);
    } else if (const auto* ink = std::get_if<domain::InkGeometry>(&annotation.geometry)) {
        xml.writeStartElement(QStringLiteral("inklist"));
        for (const auto& stroke : ink->strokes) {
            QString gesture;
            for (const domain::PointF& p : stroke) {
                if (!gesture.isEmpty()) gesture += ',';
                gesture += QString::number(p.x, 'f', 4) + ',' + QString::number(p.y, 'f', 4);
            }
            xml.writeTextElement(QStringLiteral("gesture"), gesture);
        }
        xml.writeEndElement();  // inklist
    }

    xml.writeEndElement();  // 註解元素本身
}

}  // namespace

std::string exportXfdf(const std::vector<XfdfEntry>& entries, const std::string& sourceFilename) {
    QByteArray output;
    QXmlStreamWriter xml(&output);
    xml.setAutoFormatting(true);
    xml.writeStartDocument(QStringLiteral("1.0"));
    xml.writeStartElement(QStringLiteral("xfdf"));
    xml.writeDefaultNamespace(QStringLiteral("http://ns.adobe.com/xfdf/"));

    if (!sourceFilename.empty()) {
        xml.writeStartElement(QStringLiteral("f"));
        xml.writeAttribute(QStringLiteral("href"), QString::fromStdString(sourceFilename));
        xml.writeEndElement();
    }

    xml.writeStartElement(QStringLiteral("annots"));
    for (const XfdfEntry& entry : entries) writeAnnotationElement(xml, entry);
    xml.writeEndElement();  // annots

    xml.writeEndElement();  // xfdf
    xml.writeEndDocument();
    return std::string(output.constData(), static_cast<std::size_t>(output.size()));
}

XfdfImportResult importXfdf(const std::string& xml) {
    XfdfImportResult result{};

    if (xml.size() > kMaxInputBytes) {
        result.diagnostic = "XFDF 檔案超過大小上限";
        return result;
    }
    if (containsDoctype(xml)) {
        // 不進一步剖析:光是偵測到 DOCTYPE 就足以構成拒絕理由，
        // 不需要區分它是不是真的用於實體展開攻擊。
        result.diagnostic = "拒絕含 DOCTYPE 宣告的 XFDF(可能的 XXE / 實體展開攻擊)";
        return result;
    }

    QXmlStreamReader reader(QString::fromStdString(xml));
    // QXmlStreamReader 預設不解析外部實體、也不讀取網路資源，這裡的
    // DOCTYPE 前置檢查是進一步的縱深防禦，不是唯一防線。

    domain::Annotation current;
    bool inAnnotation = false;
    std::int32_t currentPage = 0;
    QString currentElementName;
    bool inInklist = false;
    std::vector<std::vector<domain::PointF>> currentStrokes;

    const auto typeFromElement = [](const QString& name) -> std::optional<domain::AnnotationType> {
        if (name == "highlight") return domain::AnnotationType::Highlight;
        if (name == "underline") return domain::AnnotationType::Underline;
        if (name == "strikeout") return domain::AnnotationType::StrikeOut;
        if (name == "squiggly") return domain::AnnotationType::Squiggly;
        if (name == "square") return domain::AnnotationType::Square;
        if (name == "circle") return domain::AnnotationType::Circle;
        if (name == "line") return domain::AnnotationType::Line;
        if (name == "ink") return domain::AnnotationType::Ink;
        if (name == "text") return domain::AnnotationType::Text;
        if (name == "freetext") return domain::AnnotationType::FreeText;
        return std::nullopt;
    };

    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = reader.name().toString();
            if (name == "inklist") {
                inInklist = true;
                currentStrokes.clear();
                continue;
            }
            if (name == "gesture" && inInklist) {
                const std::vector<double> v = parseNumberList(reader.readElementText());
                std::vector<domain::PointF> stroke;
                for (std::size_t i = 0; i + 2 <= v.size(); i += 2) stroke.push_back({v[i], v[i + 1]});
                if (!stroke.empty()) currentStrokes.push_back(std::move(stroke));
                continue;
            }
            if (name == "quadpoints" && inAnnotation) {
                const std::vector<domain::QuadPoint> quads = parseQuadPoints(reader.readElementText());
                if (auto* markup = std::get_if<domain::TextMarkupGeometry>(&current.geometry)) {
                    markup->quads = quads;
                }
                continue;
            }
            if (name == "contents" && inAnnotation) {
                current.contents = reader.readElementText().toStdString();
                continue;
            }

            const std::optional<domain::AnnotationType> type = typeFromElement(name);
            if (type.has_value()) {
                inAnnotation = true;
                current = domain::Annotation{};
                currentElementName = name;
                switch (*type) {
                    case domain::AnnotationType::Highlight:
                        current.geometry = domain::TextMarkupGeometry{domain::TextMarkupKind::Highlight, {}};
                        break;
                    case domain::AnnotationType::Underline:
                        current.geometry = domain::TextMarkupGeometry{domain::TextMarkupKind::Underline, {}};
                        break;
                    case domain::AnnotationType::StrikeOut:
                        current.geometry = domain::TextMarkupGeometry{domain::TextMarkupKind::StrikeOut, {}};
                        break;
                    case domain::AnnotationType::Squiggly:
                        current.geometry = domain::TextMarkupGeometry{domain::TextMarkupKind::Squiggly, {}};
                        break;
                    case domain::AnnotationType::Square:
                        current.geometry = domain::ShapeGeometry{domain::ShapeKind::Square};
                        break;
                    case domain::AnnotationType::Circle:
                        current.geometry = domain::ShapeGeometry{domain::ShapeKind::Circle};
                        break;
                    case domain::AnnotationType::Line:
                        current.geometry = domain::LineGeometry{};
                        break;
                    case domain::AnnotationType::Ink:
                        current.geometry = domain::InkGeometry{};
                        break;
                    case domain::AnnotationType::Text:
                        current.geometry = domain::TextNoteGeometry{};
                        break;
                    case domain::AnnotationType::FreeText:
                        current.geometry = domain::FreeTextGeometry{};
                        break;
                    default:
                        break;
                }

                const QXmlStreamAttributes attrs = reader.attributes();
                if (attrs.hasAttribute("page")) {
                    currentPage = attrs.value("page").toInt();
                }
                if (attrs.hasAttribute("color")) {
                    if (const auto c = hexToColor(attrs.value("color").toString())) current.color = *c;
                }
                if (attrs.hasAttribute("interior-color")) {
                    current.interiorColor = hexToColor(attrs.value("interior-color").toString());
                }
                if (attrs.hasAttribute("opacity")) {
                    current.opacity = attrs.value("opacity").toDouble();
                }
                if (attrs.hasAttribute("width")) {
                    current.border.width = attrs.value("width").toDouble();
                }
                if (attrs.hasAttribute("name")) {
                    current.id = attrs.value("name").toString().toStdString();
                }
                if (attrs.hasAttribute("title")) {
                    current.author = attrs.value("title").toString().toStdString();
                }
                if (attrs.hasAttribute("subject")) {
                    current.subject = attrs.value("subject").toString().toStdString();
                }
                if (attrs.hasAttribute("creationdate")) {
                    current.creationDate = parsePdfDate(attrs.value("creationdate").toString());
                }
                if (attrs.hasAttribute("date")) {
                    current.modifiedDate = parsePdfDate(attrs.value("date").toString());
                }
                if (attrs.hasAttribute("rect")) {
                    if (const auto r = parseRect(attrs.value("rect").toString())) current.rect = *r;
                }
                if (attrs.hasAttribute("inreplyto")) {
                    current.inReplyTo = attrs.value("inreplyto").toString().toStdString();
                }
                if (attrs.hasAttribute("line")) {
                    const std::vector<double> v = parseNumberList(attrs.value("line").toString());
                    if (v.size() == 4) {
                        if (auto* line = std::get_if<domain::LineGeometry>(&current.geometry)) {
                            line->start = {v[0], v[1]};
                            line->end = {v[2], v[3]};
                        }
                    }
                }
                if (attrs.hasAttribute("fontsize")) {
                    if (auto* ft = std::get_if<domain::FreeTextGeometry>(&current.geometry)) {
                        ft->fontSize = attrs.value("fontsize").toDouble();
                    }
                }
                continue;
            }

            // 辨識到未支援的元素(表單欄位、附件、書籤等,不論是 <annots> 直屬
            // 還是巢狀在某則註解底下)，記錄但不中止整批匯入——IL-4 要求失敗要
            // 看得見,但這裡的「失敗」是局部的,不該讓一個不支援的元素拖垮
            // 其餘已經解析成功的註解。
            if (name != "xfdf" && name != "annots" && name != "f") {
                result.skippedElements.push_back(name.toStdString());
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QString name = reader.name().toString();
            if (name == "inklist") {
                inInklist = false;
                if (auto* ink = std::get_if<domain::InkGeometry>(&current.geometry)) {
                    ink->strokes = currentStrokes;
                }
                continue;
            }
            if (inAnnotation && name == currentElementName) {
                result.entries.push_back(XfdfEntry{currentPage, current});
                inAnnotation = false;
            }
        }
    }

    if (reader.hasError()) {
        result.ok = false;
        result.diagnostic = reader.errorString().toStdString();
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::app
