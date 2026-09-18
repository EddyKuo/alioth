#include "app/signature_controller.h"

#include "engine/signature/signature_clear.h"

#include <QCryptographicHash>
#include <QFile>
#include <QMetaObject>

#include <algorithm>
#include <cstring>

#include "engine/signature/signature_creator.h"
#include "engine/signature/signing_identity.h"
#include "platform/atomic_file.h"

namespace alioth::app {

SignatureController::SignatureController(QObject* parent)
    : QObject(parent), scanner_(std::make_unique<engine::signature::SignatureScanner>()) {}

SignatureController::~SignatureController() {
    // 掃描執行緒的回呼會碰本物件的成員，先關掉它再讓其餘成員解構。
    scanner_.reset();
}

void SignatureController::openDocument(const QString& path, const QString& password) {
    reports_.clear();
    open_ = false;
    verifying_ = false;

    scanner_->open(path.toStdString(), password.toStdString(),
                   [this](domain::DocumentError error) {
                       const bool ok = error == domain::DocumentError::None;
                       QMetaObject::invokeMethod(
                           this, [this, ok] { open_ = ok; }, Qt::QueuedConnection);
                   });
}

void SignatureController::closeDocument() {
    reports_.clear();
    open_ = false;
    verifying_ = false;
    scanner_->close();
}

std::int32_t SignatureController::signatureCount() const { return scanner_->signatureCount(); }

SignatureController::ClearOutcome SignatureController::clearSignatures(const QString& path) {
    ClearOutcome outcome;

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        outcome.message = tr("無法讀取檔案：%1").arg(source.errorString());
        return outcome;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    outcome.previousSize = static_cast<quint64>(bytes.size());
    {
        constexpr int kWindow = 4096;
        const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
        outcome.boundaryGuard =
            QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
    }

    const engine::signature::ClearSignaturesResult cleared =
        engine::signature::clearSignatureFields(
            std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (!cleared.ok) {
        outcome.message = tr("清除失敗：%1").arg(QString::fromStdString(cleared.diagnostic));
        return outcome;
    }
    if (!cleared.changedAnything()) {
        // 沒有簽章欄位就不寫檔。空的附加段只會讓檔案長大而使用者什麼都沒得到。
        outcome.message = tr("這份文件沒有簽章欄位");
        return outcome;
    }

    // 純附加的前提要驗過才寫檔，與其他寫入路徑同一條規則。
    if (cleared.bytes.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(cleared.bytes.data(), bytes.constData(),
                    static_cast<std::size_t>(bytes.size())) != 0) {
        outcome.message = tr("儲存結果不是增量，已中止");
        return outcome;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(cleared.bytes.data(), cleared.bytes.size()) ||
        !writer.commit()) {
        outcome.message = tr("寫檔失敗");
        return outcome;
    }

    outcome.ok = true;
    outcome.removedFields = cleared.removedFields;
    outcome.message = tr("已清除 %1 個簽章欄位（%2 個 widget）")
                          .arg(cleared.removedFields)
                          .arg(cleared.removedWidgets);
    return outcome;
}

SignatureController::SigningOutcome SignatureController::signDocument(
    const QString& path, const SigningRequest& request) {
    SigningOutcome outcome;

    if (!request.timestampUrl.isEmpty()) {
        // 時間戳（PRD-SIG-006）需要一條 TSA 的 HTTP 傳輸，那還沒接上。
        // 靜默簽一份沒有時間戳的簽章是「看起來對但不符期待」，IL-4 不允許——
        // 使用者要的是長期可驗證，拿到的卻是簽署時間由簽署者自己宣告的簽章。
        outcome.message = tr("尚未支援時間戳（TSA）。請清空時間戳網址後再簽署。");
        return outcome;
    }

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        outcome.message = tr("無法讀取檔案：%1").arg(source.errorString());
        return outcome;
    }
    const QByteArray bytes = source.readAll();
    source.close();

    outcome.previousSize = static_cast<quint64>(bytes.size());
    {
        constexpr int kWindow = 4096;
        const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
        outcome.boundaryGuard =
            QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
    }

    QFile keyFile(request.pkcs12Path);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        outcome.message = tr("無法讀取憑證檔：%1").arg(keyFile.errorString());
        return outcome;
    }
    QByteArray pkcs12 = keyFile.readAll();
    keyFile.close();

    engine::signature::SigningIdentity identity;
    const QByteArray password = request.password.toUtf8();
    const bool loaded = identity.loadPkcs12(reinterpret_cast<const std::uint8_t*>(pkcs12.constData()),
                                            static_cast<std::size_t>(pkcs12.size()),
                                            std::string(password.constData(),
                                                        static_cast<std::size_t>(password.size())));
    // 私鑰位元組與密碼用完立刻抹掉。留在堆積上等著被別的配置覆寫，
    // 等於把私鑰交給下一個要記憶體的人。
    pkcs12.fill('\0');
    if (!loaded || !identity.valid()) {
        // 不區分「密碼錯」與「檔案壞」：兩者都會讓攻擊者以錯誤訊息當成
        // 密碼正確與否的預言機。
        outcome.message = tr("無法載入憑證：檔案格式不符或密碼錯誤");
        return outcome;
    }

    engine::signature::CreateSignatureOptions options;
    options.reason = request.reason.toStdString();
    options.location = request.location.toStdString();
    options.contactInfo = request.contactInfo.toStdString();
    // 欄位名稱必須唯一，否則第二個簽章會覆蓋第一個的 AcroForm 欄位。
    options.fieldName = "Signature" + std::to_string(signatureCount() + 1);
    options.documentAlreadyHasSignatures = signatureCount() > 0;

    const engine::signature::CreateSignatureResult created = engine::signature::createSignature(
        std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())), identity, options);
    if (!created.ok) {
        outcome.message = tr("簽署失敗：%1").arg(QString::fromStdString(created.diagnostic));
        return outcome;
    }

    // 增量附加的驗收：原檔前綴必須逐位元組不變，否則既有簽章會掉成無效。
    if (created.bytes.size() < static_cast<std::size_t>(bytes.size()) ||
        std::memcmp(created.bytes.data(), bytes.constData(),
                    static_cast<std::size_t>(bytes.size())) != 0) {
        outcome.message = tr("簽署結果不是增量：原檔位元組已被改寫，已中止");
        return outcome;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(created.bytes.data(), created.bytes.size()) ||
        !writer.commit()) {
        outcome.message = tr("寫檔失敗");
        return outcome;
    }

    outcome.ok = true;
    outcome.timestamped = created.timestamped;
    outcome.message = tr("已簽署（增量 %1 位元組，簽署者 %2）")
                          .arg(created.appendedBytes)
                          .arg(QString::fromStdString(identity.subjectCommonName()));
    return outcome;
}

void SignatureController::verify() {
    if (verifying_) return;
    verifying_ = true;

    engine::signature::VerifyOptions options;
    // 吊銷查不到時給黃燈而不是綠燈。離線情境很常見，但「查不到」與「確認未吊銷」
    // 是兩件事，把前者顯示成後者正是簽章驗證最不該犯的錯。
    options.revocationPolicy = engine::signature::RevocationPolicy::SoftFail;

    scanner_->scan(&trust_, options, nullptr,
                   [this](std::vector<engine::signature::SignatureReport> reports) {
                       QMetaObject::invokeMethod(
                           this,
                           [this, reports = std::move(reports)]() mutable {
                               reports_ = std::move(reports);
                               verifying_ = false;
                               emit reportsReady();
                           },
                           Qt::QueuedConnection);
                   });
}

}  // namespace alioth::app
