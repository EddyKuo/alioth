#include "app/measurement_service.h"

#include <QFile>

#include <cstring>
#include <variant>

#include "domain/csv.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/measure_reader.h"
#include "engine/objects/measurement_writer.h"
#include "platform/atomic_file.h"

namespace alioth::app {

namespace {

using engine::objects::IncrementalAppender;
using engine::objects::PdfArray;
using engine::objects::PdfDictionary;
using engine::objects::PdfObject;
using engine::objects::PdfRef;
using engine::objects::readMeasure;
using engine::objects::SourceStatus;

QString numberFormatted(double value) {
    QString text = QString::number(value, 'f', 4);
    // 去尾零與可能落單的小數點：量測數字不需要固定四位小數，
    // 但也不能用有效位數格式（'g'）——那會在大數值時跳成科學記號，
    // 工程審圖的使用者看到 "1.2e3 mm" 只會覺得軟體壞了。
    //
    // 只在小數點之後才去尾零："0.0000" 這種整數值若不先定位小數點，
    // 去尾零迴圈會一路吃到唯一剩下的那個 "0"，變成空字串。
    const int dot = text.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        while (text.size() > dot + 1 && text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    }
    return text;
}

// /Measure 的 /A 單位標籤是 ASCII 的 "mm2" 這類形式（見 domain::measureArea），
// 顯示給使用者看時換成上標平方比較不像打字錯誤。
QString displayUnitLabel(const std::string& unitLabel, bool isArea) {
    QString unit = QString::fromStdString(unitLabel);
    if (isArea && unit.endsWith(QLatin1Char('2'))) {
        unit.chop(1);
        unit += QChar(0x00B2);
    }
    return unit;
}

const char* kindLabel(MeasurementKind kind) {
    switch (kind) {
        case MeasurementKind::Distance:  return "Distance";
        case MeasurementKind::Perimeter: return "Perimeter";
        case MeasurementKind::Area:      return "Area";
    }
    return "Distance";
}

// 找出某頁第 indexOnPage 則（依 /Annots 陣列順序）註解的已解出字典，
// 供 readExistingScale 使用。回傳的 PdfObject 若不是字典代表找不到或越界。
PdfObject annotationDictAt(const IncrementalAppender& appender, int pageIndex, int indexOnPage) {
    const auto& pages = appender.source().pages();
    if (pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= pages.size()) return PdfObject{};
    const PdfRef pageRef = pages[static_cast<std::size_t>(pageIndex)];
    const PdfObject pageObject = appender.source().object(pageRef.number);
    const PdfDictionary* pageDict = pageObject.asDictionary();
    if (pageDict == nullptr) return PdfObject{};
    const PdfObject* annotsField = pageDict->find("Annots");
    if (annotsField == nullptr) return PdfObject{};
    const PdfObject annotsArray = appender.source().resolve(*annotsField);
    const PdfArray* array = annotsArray.asArray();
    if (array == nullptr || indexOnPage < 0 ||
        static_cast<std::size_t>(indexOnPage) >= array->size()) {
        return PdfObject{};
    }
    return appender.source().resolve(array->at(static_cast<std::size_t>(indexOnPage)));
}

}  // namespace

MeasurementService::MeasurementService(QObject* parent) : QObject(parent) {}

CalibrationOutcome MeasurementService::calibrate(const CalibrationRequest& request) {
    CalibrationOutcome outcome{};
    const double pointDistance = domain::pointDistance(request.pointA, request.pointB);
    const domain::CalibrationResult result =
        domain::calibrateUniform(pointDistance, request.realDistance, request.unit);
    outcome.ok = result.ok;
    outcome.measure = result.measure;
    outcome.message = result.ok ? tr("已校正比例 %1").arg(QString::fromStdString(result.measure.ratioLabel))
                                : QString::fromStdString(result.diagnostic);
    return outcome;
}

domain::Annotation MeasurementService::applyMeasurement(domain::Annotation annotation,
                                                         const domain::MeasureInfo& scale,
                                                         bool* ok, QString* message) {
    auto fail = [&](const QString& reason) {
        if (ok != nullptr) *ok = false;
        if (message != nullptr) *message = reason;
        return annotation;
    };

    domain::MeasurementResult measured{};
    MeasurementKind kind = MeasurementKind::Distance;
    bool isArea = false;

    if (const auto* line = std::get_if<domain::LineGeometry>(&annotation.geometry)) {
        measured = domain::measureDistance(line->start, line->end, scale);
        kind = MeasurementKind::Distance;
    } else if (const auto* polygon = std::get_if<domain::PolygonGeometry>(&annotation.geometry)) {
        measured = domain::measureArea(polygon->vertices, scale);
        kind = MeasurementKind::Area;
        isArea = true;
    } else if (const auto* polyline = std::get_if<domain::PolyLineGeometry>(&annotation.geometry)) {
        measured = domain::measurePerimeter(polyline->vertices, scale, /*closed=*/false);
        kind = MeasurementKind::Perimeter;
    } else {
        return fail(tr("此註解型別不支援量測轉換（僅 Line／Polygon／PolyLine）"));
    }

    if (!measured.ok) {
        return fail(QString::fromStdString(measured.diagnostic));
    }

    QString label = numberFormatted(measured.value) + QLatin1Char(' ') +
                    displayUnitLabel(measured.unitLabel, isArea);
    if (kind == MeasurementKind::Area && measured.selfIntersecting) {
        // 自相交多邊形的面積不是視覺上圈起來的面積（domain::computePolygonArea 的
        // 說明）。數值仍然寫出去（確定性、可重現），但標籤與 message 都要讓
        // 使用者看見這個警告，不能只在服務層的回傳值裡悄悄帶過。
        label += tr("（自相交，數值僅供參考）");
    }

    annotation.contents = label.toStdString();
    annotation.measure = scale;

    if (ok != nullptr) *ok = true;
    if (message != nullptr) {
        *message = kind == MeasurementKind::Area && measured.selfIntersecting
                       ? tr("多邊形自相交，面積數值可能與視覺範圍不符，請人工複核")
                       : tr("已計算%1：%2").arg(QString::fromUtf8(kindLabel(kind)), label);
    }
    return annotation;
}

bool MeasurementService::clearMeasurementValue(const QString& path, int annotationObjectNumber,
                                               QString* message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (message != nullptr) *message = tr("無法讀取檔案：%1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    IncrementalAppender appender;
    std::string diagnostic;
    if (appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                      &diagnostic) != SourceStatus::Ok) {
        if (message != nullptr) {
            *message = tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        }
        return false;
    }

    const auto cleared = engine::objects::clearMeasurementValue(appender, annotationObjectNumber);
    if (!cleared.ok) {
        if (message != nullptr) *message = QString::fromStdString(cleared.diagnostic);
        return false;
    }

    const auto built = appender.build();
    if (!built.ok) {
        if (message != nullptr) {
            *message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        }
        return false;
    }
    const std::string& saved = built.bytes;
    if (saved.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(saved.data(), bytes.constData(), static_cast<std::size_t>(bytes.size())) != 0) {
        // 與 AnnotationService::addAnnotation 相同的邊界檢查：一旦這條不成立，
        // 既有的數位簽章會從「有效、簽章後有變更」變成「無效」（PRD-SIG-003）。
        if (message != nullptr) *message = tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章");
        return false;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(saved.data(), saved.size()) || !writer.commit()) {
        if (message != nullptr) *message = tr("寫檔失敗");
        return false;
    }
    return true;
}

domain::MeasureInfo MeasurementService::readExistingScale(const QString& path, int pageIndex,
                                                           int annotationIndexOnPage,
                                                           QString* message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (message != nullptr) *message = tr("無法讀取檔案：%1").arg(file.errorString());
        return {};
    }
    const QByteArray bytes = file.readAll();
    file.close();

    IncrementalAppender appender;
    std::string diagnostic;
    if (appender.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                      &diagnostic) != SourceStatus::Ok) {
        if (message != nullptr) {
            *message = tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        }
        return {};
    }

    const PdfObject annotDict = annotationDictAt(appender, pageIndex, annotationIndexOnPage);
    const auto result = readMeasure(appender.source(), annotDict);
    if (message != nullptr) *message = QString::fromStdString(result.diagnostic);
    return result.calibrated ? result.measure : domain::MeasureInfo{};
}

QByteArray MeasurementService::exportCsv(const std::vector<MeasurementRecord>& records) {
    std::vector<std::vector<std::string>> rows;
    rows.push_back({"Page", "Type", "AnnotationId", "Value", "Unit", "Ratio", "SelfIntersecting"});
    for (const MeasurementRecord& record : records) {
        rows.push_back({
            std::to_string(record.pageIndex),
            kindLabel(record.kind),
            record.annotationId.toStdString(),
            numberFormatted(record.value).toStdString(),
            record.unitLabel.toStdString(),
            record.ratioLabel.toStdString(),
            record.selfIntersecting ? "true" : "false",
        });
    }
    const std::string csv = domain::csvDocument(rows);
    return QByteArray(csv.data(), static_cast<qsizetype>(csv.size()));
}

}  // namespace alioth::app
