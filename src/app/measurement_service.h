#pragma once

// 量測服務（應用層，WP25：PRD-ANN-014 / 024 / 025 / 026 / 027）。
//
// 這是薄 service：計算本身全部委派給 domain/measurement.h 的純函數，
// 檔案讀寫委派給 engine/objects 的既有通道（measure_reader、
// annotation_object_writer、measurement_writer、incremental_appender）。
// 這裡只做「把使用者的一個操作串成正確的呼叫順序」。
//
// 校正比例（calibrate）本身不碰檔案：使用者在畫面上點兩下、輸入真實距離，
// 純粹是數學。真的把比例寫進某則量測註解，要等使用者用這把尺畫線／多邊形／
// 折線時，經由 applyMeasurement() 把 MeasureInfo 掛進 Annotation 再交給
// AnnotationService／writeAnnotation 寫檔——那條路徑已經在 WP0 的核心迴圈裡，
// 這裡不重造一份。

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

#include "domain/annotation.h"
#include "domain/geometry.h"
#include "domain/measurement.h"

namespace alioth::app {

struct CalibrationRequest {
    domain::PointF pointA{};
    domain::PointF pointB{};
    double realDistance{0.0};
    domain::LengthUnit unit{domain::LengthUnit::Millimetre};
};

struct CalibrationOutcome {
    bool ok{false};
    QString message;
    domain::MeasureInfo measure;
};

// 一筆量測結果，供 PRD-ANN-027 CSV 匯出使用。
enum class MeasurementKind : std::uint8_t {
    Distance,
    Perimeter,
    Area,
};

struct MeasurementRecord {
    std::int32_t pageIndex{0};
    MeasurementKind kind{MeasurementKind::Distance};
    QString annotationId;   // 對應 Annotation::id，方便使用者對照回文件
    double value{0.0};
    QString unitLabel;
    QString ratioLabel;
    bool selfIntersecting{false};  // 僅面積量測有意義，見 domain::AreaResult 的說明
};

class MeasurementService : public QObject {
    Q_OBJECT

public:
    explicit MeasurementService(QObject* parent = nullptr);

    // PRD-ANN-024：由使用者在圖面上點的兩點與輸入的真實距離推出比例尺。
    [[nodiscard]] static CalibrationOutcome calibrate(const CalibrationRequest& request);

    // PRD-ANN-026：把既有的 Line／Polygon／PolyLine 幾何套上比例尺，算出距離／
    // 面積／周長並寫進 /Contents（人類可讀的標籤），同時掛上 /Measure。
    //
    // scale 未校正時明確失敗並保留原註解不動——不會輸出一個看似合理的假數字。
    // 型別與 geometry 不符（例如把矩形註解丟進來）也明確失敗。
    [[nodiscard]] static domain::Annotation applyMeasurement(domain::Annotation annotation,
                                                              const domain::MeasureInfo& scale,
                                                              bool* ok = nullptr,
                                                              QString* message = nullptr);

    // PRD-ANN-025：清除既有量測註解的顯示值（/Contents），/Measure 與幾何不動。
    // annotationObjectNumber 是該註解在 PDF 物件層的物件編號。
    [[nodiscard]] bool clearMeasurementValue(const QString& path, int annotationObjectNumber,
                                             QString* message = nullptr);

    // 讀取既有文件中某則量測註解目前的 /Measure（例如使用者要在已校正過的
    // 圖面上追加新的量測時，沿用既有比例而不必重新校正）。
    // 找不到或非等向縮放時 measure.isCalibrated() 為 false，message 說明原因。
    [[nodiscard]] static domain::MeasureInfo readExistingScale(const QString& path,
                                                                int pageIndex,
                                                                int annotationIndexOnPage,
                                                                QString* message = nullptr);

    // PRD-ANN-027：匯出成 CSV（RFC 4180），欄位：頁碼、類型、註解 ID、數值、
    // 單位、比例、是否自相交。回傳 UTF-8 位元組，含表頭列。
    [[nodiscard]] static QByteArray exportCsv(const std::vector<MeasurementRecord>& records);
};

}  // namespace alioth::app
