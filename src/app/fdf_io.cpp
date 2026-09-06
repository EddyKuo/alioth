#include "app/fdf_io.h"

#include <algorithm>
#include <map>
#include <optional>

#include "engine/objects/annotation_reader.h"
#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_parser.h"

namespace alioth::app {
namespace {

using engine::objects::PdfArray;
using engine::objects::PdfDictionary;
using engine::objects::PdfObject;
using engine::objects::PdfParser;
using engine::objects::PdfRef;
using engine::objects::PdfString;

PdfObject numberArray(const std::vector<double>& values) {
    PdfArray array;
    array.reserve(values.size());
    for (const double value : values) array.emplace_back(value);
    return PdfObject{std::move(array)};
}

PdfObject rectArray(const domain::RectF& rect) {
    const domain::RectF r = rect.normalized();
    return numberArray({r.left, r.bottom, r.right, r.top});
}

PdfObject colorArray(const domain::ColorRgb& color) {
    return numberArray({color.r, color.g, color.b});
}

void writeGeometry(PdfDictionary& dict, const domain::Annotation& annotation) {
    if (const auto* markup = std::get_if<domain::TextMarkupGeometry>(&annotation.geometry)) {
        std::vector<double> quads;
        quads.reserve(markup->quads.size() * 8);
        for (const domain::QuadPoint& quad : markup->quads) {
            for (const domain::PointF& point : {quad.upperLeft, quad.upperRight, quad.lowerLeft,
                                                quad.lowerRight}) {
                quads.push_back(point.x);
                quads.push_back(point.y);
            }
        }
        dict.set("QuadPoints", numberArray(quads));
    } else if (const auto* line = std::get_if<domain::LineGeometry>(&annotation.geometry)) {
        dict.set("L", numberArray({line->start.x, line->start.y, line->end.x, line->end.y}));
    } else if (const auto* ink = std::get_if<domain::InkGeometry>(&annotation.geometry)) {
        PdfArray strokes;
        for (const auto& stroke : ink->strokes) {
            std::vector<double> flat;
            flat.reserve(stroke.size() * 2);
            for (const domain::PointF& point : stroke) {
                flat.push_back(point.x);
                flat.push_back(point.y);
            }
            strokes.push_back(numberArray(flat));
        }
        dict.set("InkList", PdfObject{std::move(strokes)});
    } else if (const auto* polygon = std::get_if<domain::PolygonGeometry>(&annotation.geometry)) {
        std::vector<double> flat;
        flat.reserve(polygon->vertices.size() * 2);
        for (const domain::PointF& point : polygon->vertices) {
            flat.push_back(point.x);
            flat.push_back(point.y);
        }
        dict.set("Vertices", numberArray(flat));
    }
}

PdfObject annotationDictionary(const FdfEntry& entry) {
    const domain::Annotation& annotation = entry.annotation;
    PdfDictionary dict;
    dict.set("Type", engine::objects::makeName("Annot"));
    dict.set("Subtype", engine::objects::makeName(domain::subtypeName(annotation.type())));
    // FDF 用 /Page（0 起算）指頁碼，不是 PDF 註解裡的 /P 間接參照——
    // FDF 沒有頁面物件可以指。寫成 /P 的 FDF 在 Acrobat 裡會整批落到第一頁。
    dict.set("Page", PdfObject{static_cast<std::int64_t>(entry.pageIndex)});
    dict.set("Rect", rectArray(annotation.rect));
    dict.set("C", colorArray(annotation.color));
    dict.set("CA", PdfObject{annotation.opacity});
    if (annotation.interiorColor.has_value()) {
        dict.set("IC", colorArray(*annotation.interiorColor));
    }
    {
        PdfDictionary border;
        border.set("W", PdfObject{annotation.border.width});
        dict.set("BS", PdfObject{std::move(border)});
    }
    if (!annotation.id.empty()) dict.set("NM", engine::objects::makeLiteralString(annotation.id));
    if (!annotation.author.empty()) {
        dict.set("T", engine::objects::makeTextString(annotation.author));
    }
    if (!annotation.contents.empty()) {
        dict.set("Contents", engine::objects::makeTextString(annotation.contents));
    }
    if (!annotation.subject.empty()) {
        dict.set("Subj", engine::objects::makeTextString(annotation.subject));
    }
    if (const std::string created = domain::toPdfDateString(annotation.creationDate);
        !created.empty()) {
        dict.set("CreationDate", engine::objects::makeLiteralString(created));
    }
    if (const std::string modified = domain::toPdfDateString(annotation.modifiedDate);
        !modified.empty()) {
        dict.set("M", engine::objects::makeLiteralString(modified));
    }
    if (const auto* freeText = std::get_if<domain::FreeTextGeometry>(&annotation.geometry)) {
        // /DA 是 FreeText 的必填鍵；沒有它 Acrobat 會拒絕顯示文字。
        dict.set("DA", engine::objects::makeLiteralString(
                           "/Helv " + engine::objects::formatReal(freeText->fontSize) + " Tf 0 g"));
    }
    writeGeometry(dict, annotation);
    return PdfObject{std::move(dict)};
}

}  // namespace

std::string exportFdf(const std::vector<FdfEntry>& entries, const std::string& sourceFilename) {
    // 物件 1 是 FDF 目錄，註解從 2 開始。
    std::string out = "%FDF-1.2\n";
    PdfArray annots;
    int number = 2;
    std::string body;
    for (const FdfEntry& entry : entries) {
        annots.emplace_back(PdfRef{number, 0});
        body += engine::objects::serializeIndirect(number, 0, annotationDictionary(entry));
        ++number;
    }

    PdfDictionary fdf;
    if (!sourceFilename.empty()) {
        fdf.set("F", engine::objects::makeTextString(sourceFilename));
    }
    fdf.set("Annots", PdfObject{std::move(annots)});
    PdfDictionary root;
    root.set("FDF", PdfObject{std::move(fdf)});

    out += engine::objects::serializeIndirect(1, 0, PdfObject{std::move(root)});
    out += body;

    // FDF 的 trailer 不帶交叉參照表（ISO 32000-1 §12.7.7.1）——這不是省略，
    // 規格明定 FDF 不需要 xref，寫一個進去反而讓部分工具判成損毀的 PDF。
    PdfDictionary trailer;
    trailer.set("Root", PdfObject{PdfRef{1, 0}});
    out += "trailer\n";
    out += engine::objects::serialize(PdfObject{std::move(trailer)});
    out += "\n%%EOF\n";
    return out;
}

FdfImportResult importFdf(const std::string& bytes) {
    FdfImportResult result;
    if (bytes.size() > kMaxFdfBytes) {
        result.diagnostic = "FDF 超過 " + std::to_string(kMaxFdfBytes / (1024 * 1024)) + " MB 上限";
        return result;
    }
    if (bytes.rfind("%FDF-", 0) != 0) {
        result.diagnostic = "不是 FDF：檔案開頭沒有 %FDF-";
        return result;
    }

    // 順序掃過所有 N G obj … endobj，不去讀 trailer 也不建 xref。
    //
    // FDF 規格本來就沒有交叉參照表，而現實中的 FDF 有的連 trailer 都寫壞；
    // 靠 /Root 找目錄會讓一堆能讀的檔案讀不進來。改成「找出唯一帶 /FDF 的字典」
    // ——那是 FDF 目錄的定義性特徵，比 trailer 可靠。
    std::map<int, PdfObject> objects;
    PdfParser parser(bytes);
    while (true) {
        if (!parser.skipWhitespace()) break;
        const std::size_t before = parser.position();
        int number = 0;
        int generation = 0;
        PdfObject object;
        if (parser.parseIndirectObject(number, generation, object)) {
            objects.emplace(number, std::move(object));
            continue;
        }
        // 解析不動就往前挪一個位元組再試：檔案裡夾雜的註解、trailer、%%EOF
        // 都不是物件，跳過它們比在第一個非物件位元組上放棄好。
        parser.seek(before + 1);
        if (before + 1 >= bytes.size()) break;
    }

    const PdfDictionary* fdf = nullptr;
    for (const auto& [objectNumber, object] : objects) {
        const PdfDictionary* dict = object.asDictionary();
        if (dict == nullptr) continue;
        if (const PdfObject* value = dict->find("FDF"); value != nullptr) {
            if (const PdfDictionary* inner = value->asDictionary()) {
                fdf = inner;
                break;
            }
        }
    }
    if (fdf == nullptr) {
        result.diagnostic = "找不到 FDF 目錄（沒有任何物件帶 /FDF）";
        return result;
    }

    if (fdf->has("JavaScript")) {
        // 不執行、也不保留。明確說出來，讓使用者知道那份檔案帶了什麼——
        // 靜默丟掉會讓「為什麼對方的自動計算沒作用」變成無從查起的問題。
        result.skipped.emplace_back("/JavaScript（本產品不執行 PDF 內嵌 JavaScript）");
    }

    const PdfObject* annotsValue = fdf->find("Annots");
    if (annotsValue == nullptr) {
        result.ok = true;  // 合法的空 FDF（例如只帶表單資料），不是錯誤
        return result;
    }
    const PdfArray* annots = annotsValue->asArray();
    if (annots == nullptr) {
        result.diagnostic = "/Annots 不是陣列";
        return result;
    }

    for (const PdfObject& item : *annots) {
        const PdfDictionary* dict = nullptr;
        if (item.isRef()) {
            const auto it = objects.find(item.asRef().number);
            if (it != objects.end()) dict = it->second.asDictionary();
        } else {
            dict = item.asDictionary();
        }
        if (dict == nullptr) {
            result.skipped.emplace_back("無法解析的 /Annots 項目");
            continue;
        }

        // 字典 → 領域模型的規則只有一份（engine/objects/annotation_reader）。
        // FDF 的註解字典與 PDF 裡的是同一套鍵，另寫一份遲早會在某個鍵上分岔，
        // 而症狀是「同一則註解從 PDF 讀出來與從 FDF 讀出來不一樣」。
        std::optional<domain::Annotation> annotation =
            engine::objects::readAnnotation(*dict);
        if (!annotation.has_value()) {
            std::string subtype;
            if (const PdfObject* value = dict->find("Subtype");
                value != nullptr && value->isName()) {
                subtype = value->asName();
            }
            result.skipped.emplace_back(subtype.empty() ? "缺少 /Subtype" : "/" + subtype);
            continue;
        }

        FdfEntry entry;
        // FDF 專屬的一個鍵：頁碼。PDF 的註解靠 /P 指向頁面物件，FDF 沒有頁面
        // 可指，改用 0 起算的 /Page，所以它不在共用的 readAnnotation 裡。
        if (const PdfObject* page = dict->find("Page"); page != nullptr && page->isNumber()) {
            entry.pageIndex = static_cast<std::int32_t>(page->asInteger());
        }
        // 頁碼為負代表檔案壞了，不是「最後一頁」之類的慣例。當成 0 會把註解
        // 悄悄放到第一頁，那比跳過更難發現。
        if (entry.pageIndex < 0) {
            result.skipped.emplace_back("/Page 為負數的註解");
            continue;
        }
        entry.annotation = std::move(*annotation);
        result.entries.push_back(std::move(entry));
    }

    result.ok = true;
    if (!result.skipped.empty()) {
        result.diagnostic = "已匯入 " + std::to_string(result.entries.size()) + " 則，跳過 " +
                            std::to_string(result.skipped.size()) + " 項";
    }
    return result;
}

}  // namespace alioth::app
