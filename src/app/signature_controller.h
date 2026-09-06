#pragma once

// 簽章驗證控制器（PRD-SIG-001/002）。
//
// 簽章面板是 Persona P2（法務審閱者）契合度最高的功能，而它的正確性判準只有一條：
// **不受信任的東西不可以看起來像受信任的**。因此這一層刻意不做任何「簡化」——
// 三態燈號原樣傳給 UI，findings 逐條傳給 UI，不合併也不摘要。
//
// 掃描很慢（要算整份檔案的雜湊、可能還要連網查吊銷），所以走背景執行緒；
// SignatureScanner 自己持有把手與執行緒，這裡只負責編組回 GUI。

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

#include "engine/signature/signature_scanner.h"
#include "engine/signature/signature_types.h"
#include "engine/signature/trust_store.h"

namespace alioth::app {

class SignatureController : public QObject {
    Q_OBJECT

public:
    explicit SignatureController(QObject* parent = nullptr);
    ~SignatureController() override;

    void openDocument(const QString& path, const QString& password = {});
    void closeDocument();

    // 開始驗證。結果以 reportsReady 送出。
    void verify();

    [[nodiscard]] const std::vector<engine::signature::SignatureReport>& reports() const noexcept {
        return reports_;
    }
    [[nodiscard]] std::int32_t signatureCount() const;
    [[nodiscard]] bool isVerifying() const noexcept { return verifying_; }

    // 信任存放區。空的存放區代表「什麼都不信任」，結果會是黃燈而不是綠燈——
    // 那是正確的預設：憑空信任一條無法回溯的憑證鏈比報錯更危險。
    [[nodiscard]] engine::signature::TrustStore& trustStore() noexcept { return trust_; }

    // 簽署一份文件（PRD-SIG-004）。
    //
    // 身分來自使用者選的 PKCS#12（.pfx/.p12）與它的密碼。密碼只在這個呼叫的
    // 期間存在於記憶體，不寫進設定、不進 log、也不留在任何成員變數上——
    // 私鑰密碼一旦落到磁碟或訊息裡就等於私鑰本身外洩。
    //
    // 寫入走增量附加：原檔一個位元組都不動，既有簽章仍然是「有效，簽章後
    // 有變更」而不是無效。
    struct SigningRequest {
        QString pkcs12Path;
        QString password;
        QString reason;
        QString location;
        QString contactInfo;
        // 空字串代表不要時間戳。要了卻拿不到時整個簽署失敗，不會靜默退回
        // 一份沒有時間戳的簽章（IL-4）。
        QString timestampUrl;
    };

    struct SigningOutcome {
        bool ok{false};
        QString message;
        quint64 previousSize{0};   // 復原點：增量附加是純附加，截回這裡即可
        QByteArray boundaryGuard;  // 邊界守衛，與註解服務同一套
        bool timestamped{false};
    };

    [[nodiscard]] SigningOutcome signDocument(const QString& path, const SigningRequest& request);

signals:
    void reportsReady();
    void verifyFailed(const QString& message);

private:
    std::unique_ptr<engine::signature::SignatureScanner> scanner_;
    engine::signature::TrustStore trust_;
    std::vector<engine::signature::SignatureReport> reports_;
    bool open_{false};
    bool verifying_{false};
};

}  // namespace alioth::app
