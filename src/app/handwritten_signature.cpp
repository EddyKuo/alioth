#include "app/handwritten_signature.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace alioth::app {
namespace {

constexpr const char* kKey = "signatures/library";

// 正規化座標 → 目標矩形。Y 軸不翻轉：領域層的頁面座標與這裡的單位方框
// 都是原點左下、Y 向上，翻轉會讓簽名上下顛倒，而那個錯誤在單元測試裡
// 只會表現成幾個數字不一樣，不容易一眼看出。
domain::PointF mapToRect(const domain::PointF& unit, const domain::RectF& rect) {
    return domain::PointF{rect.left + unit.x * rect.width(),
                          rect.bottom + unit.y * rect.height()};
}

}  // namespace

bool SavedSignature::isValid() const {
    if (name.isEmpty()) return false;
    if (kind == SignatureSourceKind::Ink) {
        if (strokes.empty()) return false;
        for (const std::vector<domain::PointF>& stroke : strokes) {
            // 單點的筆畫畫不出線段，留著只會在輸出裡產生空的 /InkList 項目。
            if (stroke.size() < 2) return false;
            for (const domain::PointF& point : stroke) {
                if (point.x < 0.0 || point.x > 1.0 || point.y < 0.0 || point.y > 1.0) return false;
            }
        }
        return true;
    }
    if (width <= 0 || height <= 0) return false;
    if (channels != 3 && channels != 4) return false;
    return pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                static_cast<std::size_t>(channels);
}

std::optional<domain::Annotation> buildSignatureAnnotation(const SavedSignature& signature,
                                                           const domain::RectF& rect,
                                                           const QString& author) {
    if (!signature.isValid()) return std::nullopt;
    const domain::RectF target = rect.normalized();
    if (target.isEmpty()) return std::nullopt;

    domain::Annotation annotation;
    annotation.rect = target;
    annotation.author = author.toStdString();
    // /Subj 是這則註解唯一能自我表明身分的地方。少了它，別的檢視器（與我們
    // 自己的簽章面板）就分不出手寫簽名與一般手繪註解。
    annotation.subject = kHandwrittenSignatureSubject;
    const QDateTime now = QDateTime::currentDateTime();
    domain::PdfDate stamped;
    stamped.year = now.date().year();
    stamped.month = now.date().month();
    stamped.day = now.date().day();
    stamped.hour = now.time().hour();
    stamped.minute = now.time().minute();
    stamped.second = now.time().second();
    // 時區偏移取當地設定。手寫簽名唯一能提供的「可追溯性」就是時間，
    // 而沒有時區的時間在跨時區審閱時是誤導。
    const int offsetSeconds = now.offsetFromUtc();
    stamped.tzHours = offsetSeconds / 3600;
    stamped.tzMinutes = std::abs(offsetSeconds % 3600) / 60;
    annotation.creationDate = stamped;
    annotation.modifiedDate = stamped;
    annotation.color = domain::ColorRgb{0.05, 0.1, 0.45};  // 深藍，貼近實際簽字筆
    annotation.flags = domain::AnnotationFlag::Print;

    if (signature.kind == SignatureSourceKind::Ink) {
        domain::InkGeometry ink;
        ink.strokes.reserve(signature.strokes.size());
        for (const std::vector<domain::PointF>& stroke : signature.strokes) {
            std::vector<domain::PointF> mapped;
            mapped.reserve(stroke.size());
            for (const domain::PointF& point : stroke) mapped.push_back(mapToRect(point, target));
            ink.strokes.push_back(std::move(mapped));
        }
        // 線寬隨簽名框高度縮放：固定寬度在小欄位裡會糊成一團黑。
        annotation.border.width = std::max(0.5, target.height() * 0.03);
        annotation.geometry = std::move(ink);
        return annotation;
    }

    domain::StampGeometry stamp;
    stamp.kind = domain::StampKind::Custom;
    stamp.imagePixels = signature.pixels;
    stamp.imageWidth = signature.width;
    stamp.imageHeight = signature.height;
    stamp.imageChannels = signature.channels;
    annotation.geometry = std::move(stamp);
    return annotation;
}

bool isHandwrittenSignature(const domain::Annotation& annotation) {
    return annotation.subject == kHandwrittenSignatureSubject;
}

bool SignatureLibrary::add(const SavedSignature& signature) {
    if (!signature.isValid()) return false;

    const auto existing = std::find_if(signatures_.begin(), signatures_.end(),
                                       [&](const SavedSignature& saved) {
                                           return saved.name == signature.name;
                                       });
    if (existing != signatures_.end()) {
        *existing = signature;
        return true;
    }
    if (static_cast<int>(signatures_.size()) >= kMaxSignatures) return false;
    signatures_.push_back(signature);
    return true;
}

bool SignatureLibrary::remove(const QString& name) {
    const auto it = std::find_if(signatures_.begin(), signatures_.end(),
                                 [&](const SavedSignature& saved) { return saved.name == name; });
    if (it == signatures_.end()) return false;
    signatures_.erase(it);
    return true;
}

const SavedSignature* SignatureLibrary::find(const QString& name) const {
    for (const SavedSignature& saved : signatures_) {
        if (saved.name == name) return &saved;
    }
    return nullptr;
}

void SignatureLibrary::load() {
    signatures_.clear();
    QSettings settings;
    const QByteArray raw = settings.value(QLatin1String(kKey)).toByteArray();
    if (raw.isEmpty()) return;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) return;

    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        SavedSignature saved;
        saved.name = object.value(QStringLiteral("name")).toString();
        saved.kind = object.value(QStringLiteral("kind")).toString() == QStringLiteral("image")
                         ? SignatureSourceKind::Image
                         : SignatureSourceKind::Ink;
        if (saved.kind == SignatureSourceKind::Ink) {
            for (const QJsonValue& strokeValue :
                 object.value(QStringLiteral("strokes")).toArray()) {
                std::vector<domain::PointF> stroke;
                const QJsonArray points = strokeValue.toArray();
                // 座標成對存放，奇數長度代表資料壞了——丟掉這一筆而不是
                // 補一個猜出來的座標。
                if (points.size() % 2 != 0) continue;
                for (int i = 0; i + 1 < points.size(); i += 2) {
                    stroke.push_back(domain::PointF{points[i].toDouble(), points[i + 1].toDouble()});
                }
                if (stroke.size() >= 2) saved.strokes.push_back(std::move(stroke));
            }
        } else {
            saved.width = object.value(QStringLiteral("width")).toInt();
            saved.height = object.value(QStringLiteral("height")).toInt();
            saved.channels = object.value(QStringLiteral("channels")).toInt(3);
            const QByteArray pixels = QByteArray::fromBase64(
                object.value(QStringLiteral("pixels")).toString().toLatin1());
            saved.pixels.assign(reinterpret_cast<const std::uint8_t*>(pixels.constData()),
                                reinterpret_cast<const std::uint8_t*>(pixels.constData()) +
                                    pixels.size());
        }
        // 壞掉的項目個別跳過。為了一筆壞資料丟掉整個簽名庫，
        // 使用者會以為自己的簽名全部不見了。
        if (saved.isValid()) signatures_.push_back(std::move(saved));
        if (static_cast<int>(signatures_.size()) >= kMaxSignatures) break;
    }
}

void SignatureLibrary::save() const {
    QJsonArray array;
    for (const SavedSignature& saved : signatures_) {
        QJsonObject object;
        object[QStringLiteral("name")] = saved.name;
        if (saved.kind == SignatureSourceKind::Ink) {
            object[QStringLiteral("kind")] = QStringLiteral("ink");
            QJsonArray strokes;
            for (const std::vector<domain::PointF>& stroke : saved.strokes) {
                QJsonArray points;
                for (const domain::PointF& point : stroke) {
                    points.append(point.x);
                    points.append(point.y);
                }
                strokes.append(points);
            }
            object[QStringLiteral("strokes")] = strokes;
        } else {
            object[QStringLiteral("kind")] = QStringLiteral("image");
            object[QStringLiteral("width")] = saved.width;
            object[QStringLiteral("height")] = saved.height;
            object[QStringLiteral("channels")] = saved.channels;
            const QByteArray pixels(reinterpret_cast<const char*>(saved.pixels.data()),
                                    static_cast<qsizetype>(saved.pixels.size()));
            object[QStringLiteral("pixels")] = QString::fromLatin1(pixels.toBase64());
        }
        array.append(object);
    }
    QSettings settings;
    settings.setValue(QLatin1String(kKey), QJsonDocument(array).toJson(QJsonDocument::Compact));
}

}  // namespace alioth::app
