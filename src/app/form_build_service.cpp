#include "app/form_build_service.h"

#include <QCryptographicHash>
#include <QFile>

#include <algorithm>
#include <cstring>

#include "engine/formbuild/form_field_writer.h"
#include "engine/objects/incremental_appender.h"
#include "platform/atomic_file.h"

namespace alioth::app {
namespace {

bool readAll(const QString& path, QByteArray* out, QString* message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (message) *message = QObject::tr("無法讀取檔案：%1").arg(file.errorString());
        return false;
    }
    *out = file.readAll();
    return true;
}

QByteArray boundaryHashOf(const QByteArray& bytes) {
    constexpr int kWindow = 4096;
    const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
    return QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
}

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

[[nodiscard]] const char* defaultPrefixFor(engine::formbuild::BuildFieldType type) {
    using engine::formbuild::BuildFieldType;
    switch (type) {
        case BuildFieldType::CheckBox: return "check";
        case BuildFieldType::RadioGroup: return "radio";
        case BuildFieldType::ComboBox: return "combo";
        case BuildFieldType::ListBox: return "list";
        case BuildFieldType::PushButton: return "button";
        case BuildFieldType::Date: return "date";
        case BuildFieldType::Image: return "image";
        case BuildFieldType::Signature: return "signature";
        default: return "text";
    }
}

// 找一個不會撞到既有欄位的名字。
//
// 撞名的後果不是報錯而是**兩個欄位共用一個值**——PDF 的規則是同名欄位屬於
// 同一個欄位的多個 widget。使用者會看到打字在一格、另一格跟著變，而畫面上
// 沒有任何跡象說明為什麼。
[[nodiscard]] std::string uniqueName(const std::vector<std::string>& taken,
                                     const std::string& prefix) {
    for (int i = 1; i < 100000; ++i) {
        const std::string candidate = prefix + std::to_string(i);
        if (std::find(taken.begin(), taken.end(), candidate) == taken.end()) return candidate;
    }
    return prefix;
}

}  // namespace

FormBuildService::FormBuildService(QObject* parent) : QObject(parent) {}

QStringList FormBuildService::existingFieldNames(const QString& path) const {
    QByteArray bytes;
    QString ignored;
    if (!readAll(path, &bytes, &ignored)) return {};

    engine::objects::IncrementalAppender appender;
    if (appender.open(toStd(bytes)) != engine::objects::SourceStatus::Ok) return {};
    engine::formbuild::FormFieldWriter writer(appender);
    if (!writer.ready()) return {};

    QStringList names;
    for (const std::string& name : writer.existingFieldNames()) {
        names << QString::fromStdString(name);
    }
    return names;
}

HighlightResult FormBuildService::addField(const FormFieldRequest& request) {
    HighlightResult result;
    if (request.rectPt.normalized().isEmpty()) {
        // 空矩形的欄位在畫面上完全看不見，而且點不到——建了等於沒建，
        // 但它會出現在欄位清單裡讓人以為建成功了。
        result.message = tr("欄位範圍是空的");
        return result;
    }

    QByteArray bytes;
    if (!readAll(request.path, &bytes, &result.message)) return result;

    result.previousSize = static_cast<quint64>(bytes.size());
    result.boundaryGuard = boundaryHashOf(bytes);

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    const engine::objects::SourceStatus status = appender.open(toStd(bytes), &diagnostic);
    if (status != engine::objects::SourceStatus::Ok) {
        result.message = status == engine::objects::SourceStatus::Encrypted
                             ? tr("加密文件尚不支援建立表單欄位")
                             : tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    engine::formbuild::FormFieldWriter writer(appender);
    if (!writer.ready()) {
        result.message = tr("無法建立表單欄位：%1")
                             .arg(QString::fromStdString(writer.diagnostic()));
        return result;
    }

    engine::formbuild::FieldDefinition definition;
    definition.type = request.type;
    definition.pageIndex = request.pageIndex;
    definition.rectPt = request.rectPt.normalized();
    definition.required = request.required;
    definition.readOnly = request.readOnly;
    definition.multiline = request.multiline;
    for (const QString& option : request.options) {
        definition.options.push_back(option.toStdString());
    }

    const std::vector<std::string>& taken = writer.existingFieldNames();
    const std::string requested = request.name.trimmed().toStdString();
    if (requested.empty()) {
        definition.name = uniqueName(taken, defaultPrefixFor(request.type));
    } else if (std::find(taken.begin(), taken.end(), requested) != taken.end()) {
        // 明確拒絕而不是默默改名：使用者指定了名字，多半是因為別的地方
        // （匯出資料、計算式）會用到它，替他改掉會讓那些地方對不上。
        result.message = tr("已經有一個叫「%1」的欄位。同名欄位在 PDF 裡是同一個欄位，"
                            "兩格會共用同一個值。").arg(request.name.trimmed());
        return result;
    } else {
        definition.name = requested;
    }

    const engine::formbuild::FieldWriteResult written = writer.addField(definition);
    if (!written.ok) {
        result.message = tr("建立欄位失敗：%1").arg(QString::fromStdString(written.diagnostic));
        return result;
    }

    // finish() 回傳的是**診斷字串**，不是位元組：空字串代表成功。
    // 位元組要另外向 appender 要。這個介面容易誤讀，誤讀的症狀是每次建立
    // 欄位都「成功地什麼都沒寫」或「失敗但沒有原因」。
    if (const std::string diagnosticFromFinish = writer.finish();
        !diagnosticFromFinish.empty()) {
        result.message = tr("寫入表單失敗：%1")
                             .arg(QString::fromStdString(diagnosticFromFinish));
        return result;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }
    const std::string& output = built.bytes;

    if (output.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(output.data(), bytes.constData(),
                    static_cast<std::size_t>(bytes.size())) != 0) {
        result.message = tr("儲存結果不是增量：原檔位元組已被改寫，已中止以保全簽章");
        return result;
    }

    platform::AtomicFileWriter atomic(request.path);
    if (!atomic.begin() || !atomic.write(output.data(), output.size()) || !atomic.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.message = tr("已建立欄位「%1」").arg(QString::fromStdString(definition.name));
    return result;
}

}  // namespace alioth::app
