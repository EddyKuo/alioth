#include "engine/objects/measurement_writer.h"

namespace alioth::engine::objects {

MeasurementClearResult clearMeasurementValue(IncrementalAppender& appender,
                                             int annotationObjectNumber) {
    if (!appender.isOpen()) return {false, "附加器尚未開啟原檔"};

    PdfObject current = appender.currentObject(annotationObjectNumber);
    PdfDictionary* dict = current.asDictionary();
    if (dict == nullptr) {
        return {false, "找不到指定編號的註解字典：" + std::to_string(annotationObjectNumber)};
    }
    if (!dict->has("Measure")) {
        return {false, "這則註解沒有 /Measure，不是量測註解，拒絕清除"};
    }

    dict->remove("Contents");

    if (!appender.updateObject(annotationObjectNumber, current)) {
        return {false, "更新註解物件失敗"};
    }
    return {true, {}};
}

}  // namespace alioth::engine::objects
