#include "engine/objects/measure_reader.h"

#include <optional>
#include <utility>

namespace alioth::engine::objects {

namespace {

// 讀 /X 或 /Y：兩者格式相同，都是一個 NumberFormat 字典的陣列，量測只用第一項
// （ISO 32000-1 §12.5.6.11 允許多項是為了同一軸給多種顯示單位，我們只需要
// 換算係數，取第一項即可）。
[[nodiscard]] std::optional<std::pair<std::string, double>> readAxis(const PdfSourceDocument& source,
                                                                      const PdfDictionary& measureDict,
                                                                      const char* key) {
    const PdfObject* field = measureDict.find(key);
    if (field == nullptr) return std::nullopt;
    const PdfObject arrayObject = source.resolve(*field);
    const PdfArray* array = arrayObject.asArray();
    if (array == nullptr || array->empty()) return std::nullopt;

    const PdfObject entry = source.resolve(array->front());
    const PdfDictionary* entryDict = entry.asDictionary();
    if (entryDict == nullptr) return std::nullopt;

    const PdfObject* factorField = entryDict->find("C");
    if (factorField == nullptr) return std::nullopt;
    const double factor = factorField->asNumber(0.0);
    if (!(factor > 0.0)) return std::nullopt;

    const PdfObject* unitField = entryDict->find("U");
    std::string unit = unitField != nullptr ? unitField->asName() : std::string{};
    return std::make_pair(std::move(unit), factor);
}

// /R 恆以常值字串寫出（見 annotation_object_writer.cpp 的 measureDictionary），
// 因此讀回時只支援常值字串；十六進位字串（罕見，通常只有非 ASCII 標籤才會用）
// 原樣回傳其位元組，不嘗試解碼——比例標籤只是給人看的說明文字，
// 解錯碼顯示亂碼也不影響換算結果本身。
[[nodiscard]] std::string readRatioLabel(const PdfSourceDocument& source,
                                         const PdfDictionary& measureDict) {
    const PdfObject* field = measureDict.find("R");
    if (field == nullptr) return {};
    const PdfObject resolved = source.resolve(*field);
    if (const auto* str = std::get_if<PdfString>(&resolved.value())) return str->bytes;
    return {};
}

}  // namespace

MeasureReadResult readMeasure(const PdfSourceDocument& source, const PdfObject& annotationDict) {
    MeasureReadResult result{};

    const PdfDictionary* annot = annotationDict.asDictionary();
    if (annot == nullptr) {
        result.diagnostic = "不是有效的註解字典";
        return result;
    }

    const PdfObject* measureField = annot->find("Measure");
    if (measureField == nullptr) {
        result.diagnostic = "此標記沒有 /Measure，尚未校正比例";
        return result;
    }

    const PdfObject measureObject = source.resolve(*measureField);
    const PdfDictionary* measureDict = measureObject.asDictionary();
    if (measureDict == nullptr) {
        result.diagnostic = "/Measure 不是字典，格式無法辨識";
        return result;
    }

    if (const PdfObject* subtypeField = measureDict->find("Subtype"); subtypeField != nullptr) {
        const std::string subtype = subtypeField->asName();
        if (!subtype.empty() && subtype != "RL") {
            result.diagnostic = "不支援的量測字典 Subtype：" + subtype;
            return result;
        }
    }

    const auto xAxis = readAxis(source, *measureDict, "X");
    if (!xAxis.has_value()) {
        result.diagnostic = "/Measure 缺少可用的 /X 換算資訊";
        return result;
    }
    const auto yAxis = readAxis(source, *measureDict, "Y");

    const double xFactor = xAxis->second;
    const double yFactor = yAxis.has_value() ? yAxis->second : xFactor;
    const std::optional<double> resolved = domain::resolveUniformScale(xFactor, yFactor);
    if (!resolved.has_value()) {
        // 非等向縮放：X/Y 兩軸比例不一致，沒有單一正確的換算係數可用。
        // 明確拒絕，不挑一軸將就（domain/measurement.h 的設計理由同此）。
        result.diagnostic = "此頁為非等向縮放（X/Y 比例不一致），已拒絕自動換算";
        return result;
    }

    result.calibrated = true;
    result.measure.unitsPerPoint = *resolved;
    result.measure.unitLabel = xAxis->first;
    result.measure.ratioLabel = readRatioLabel(source, *measureDict);
    result.hasAreaFormat = measureDict->has("A");
    return result;
}

}  // namespace alioth::engine::objects
