#include "engine/pageops/annotation_transform.h"

#include <cmath>

namespace alioth::engine::pageops {
namespace {

using domain::PointF;
using domain::RectF;
using domain::compose::Matrix;

// 由 x,y 成對組成的座標陣列。/QuadPoints 是 8 個一組、/Vertices 與 /L 是點串，
// 但變換規則相同：每一對都是一個點。
void transformPointArray(PdfArray& array, const Matrix& matrix) {
    for (std::size_t i = 0; i + 1 < array.size(); i += 2) {
        const PdfObject& xValue = array[i];
        const PdfObject& yValue = array[i + 1];
        if (!xValue.isNumber() || !yValue.isNumber()) continue;
        const PointF mapped =
            domain::compose::apply(matrix, PointF{xValue.asNumber(), yValue.asNumber()});
        array[i] = PdfObject{mapped.x};
        array[i + 1] = PdfObject{mapped.y};
    }
}

// /InkList 是「陣列的陣列」，每個子陣列是一筆連續筆畫。
void transformNestedPointArray(PdfArray& array, const Matrix& matrix) {
    for (PdfObject& item : array) {
        if (PdfArray* inner = item.asArray()) transformPointArray(*inner, matrix);
    }
}

// 直接陣列與間接參照兩種形態都要處理：/QuadPoints 多半是直接陣列，
// 但沒有任何規定禁止它是參照，而漏掉那一種的症狀是「大部分檔案都對」。
PdfArray* mutableArray(PdfDocumentRewriter& document, PdfDictionary& dict, const char* key) {
    PdfObject* slot = dict.find(key);
    if (slot == nullptr) return nullptr;
    if (PdfArray* direct = slot->asArray()) return direct;
    if (!slot->isRef()) return nullptr;
    PdfObject* target = document.object(slot->asRef().number);
    return target == nullptr ? nullptr : target->asArray();
}

}  // namespace

std::vector<int> pageAnnotationObjects(const PdfDocumentRewriter& document, const PdfRef& page) {
    std::vector<int> out;
    const PdfDictionary* dict = pageDictionary(document, page);
    if (dict == nullptr) return out;
    const PdfObject* annots = dict->find("Annots");
    if (annots == nullptr) return out;
    const PdfObject resolved = document.resolve(*annots);
    const PdfArray* array = resolved.asArray();
    if (array == nullptr) return out;
    for (const PdfObject& item : *array) {
        if (item.isRef() && item.asRef().valid()) out.push_back(item.asRef().number);
    }
    return out;
}

void setPageAnnotationObjects(PdfDocumentRewriter& document, const PdfRef& page,
                              const std::vector<int>& annotations) {
    PdfDictionary* dict = pageDictionary(document, page);
    if (dict == nullptr) return;
    if (annotations.empty()) {
        dict->remove("Annots");
        return;
    }
    PdfArray array;
    array.reserve(annotations.size());
    for (const int number : annotations) array.push_back(objects::makeRef(number));
    dict->set("Annots", PdfObject{std::move(array)});
}

bool transformAnnotation(PdfDocumentRewriter& document, int annotationObject, const Matrix& matrix,
                         const PdfRef* newPage, std::set<int>& visited) {
    if (!visited.insert(annotationObject).second) return true;
    PdfObject* object = document.object(annotationObject);
    if (object == nullptr) return false;
    PdfDictionary* dict = object->asDictionary();
    if (dict == nullptr) return false;

    if (const PdfObject* rectValue = dict->find("Rect")) {
        RectF rect{};
        if (readRectArray(document, *rectValue, rect)) {
            // 用四個角算外接矩形而不是只算兩個角：矩陣含旋轉時，
            // 只算左下右上會得到一個左右顛倒的矩形，而 /Rect 必須是正規化的。
            dict->set("Rect", makeRectArray(domain::compose::apply(matrix, rect)));
        }
    }

    for (const char* key : {"QuadPoints", "Vertices", "L", "CL", "Path"}) {
        if (PdfArray* array = mutableArray(document, *dict, key)) {
            transformPointArray(*array, matrix);
        }
    }
    if (PdfArray* ink = mutableArray(document, *dict, "InkList")) {
        transformNestedPointArray(*ink, matrix);
    }

    // /RD 是「/Rect 內縮多少」的四個非負距離，不是座標，因此只縮放不平移。
    // 用矩陣的軸向縮放量取絕對值：旋轉 90 度時 a 為 0，取 |a|+|c| 才是實際的 x 縮放。
    if (PdfArray* differences = mutableArray(document, *dict, "RD")) {
        const double scaleX = std::fabs(matrix.a) + std::fabs(matrix.c);
        const double scaleY = std::fabs(matrix.b) + std::fabs(matrix.d);
        const double factors[4] = {scaleX, scaleY, scaleX, scaleY};
        for (std::size_t i = 0; i < differences->size() && i < 4; ++i) {
            if ((*differences)[i].isNumber()) {
                (*differences)[i] = PdfObject{(*differences)[i].asNumber() * factors[i]};
            }
        }
    }

    // 彈出視窗是獨立的註解物件，它的 /Rect 在同一個頁面座標系裡，
    // 不跟著搬的話便利貼的內容框會留在原位。
    if (const PdfObject* popup = dict->find("Popup")) {
        if (popup->isRef() && popup->asRef().valid()) {
            const int number = popup->asRef().number;
            (void)transformAnnotation(document, number, matrix, newPage, visited);
        }
    }

    if (newPage != nullptr && dict->has("P")) {
        dict->set("P", objects::makeRef(newPage->number, newPage->generation));
    }
    return true;
}

}  // namespace alioth::engine::pageops
