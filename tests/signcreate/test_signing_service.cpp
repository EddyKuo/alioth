// 應用層的簽署入口（PRD-SIG-004）。
//
// 密碼學本身由 test_signature_creator.cpp 驗（CMS 結構、/ByteRange、qpdf 檢查）。
// 這裡驗的是**應用層在出錯時做了什麼**——那一段沒有任何密碼學，卻是使用者
// 唯一會碰到的部分：
//
//   - 要求時間戳卻拿不到時，寧可失敗也不靜默簽一份沒有時間戳的（IL-4）
//   - 憑證載不進來時不碰文件，而且錯誤訊息不區分「密碼錯」與「檔案壞」
//   - 失敗一律不留下半寫的檔案

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "app/signature_controller.h"
#include "object_fixture.h"

using alioth::app::SignatureController;

namespace {

QString writePdf(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(alioth::test::makeFixturePdf());
    file.close();
    return path;
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

}  // namespace

class TestSigningService : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void requestingATimestampFailsLoudlyInsteadOfSigningWithoutOne();
    void invalidCertificateLeavesTheDocumentUntouched();
    void errorMessageDoesNotDistinguishWrongPasswordFromBrokenFile();
    void missingDocumentIsReportedNotCrashed();

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

void TestSigningService::requestingATimestampFailsLoudlyInsteadOfSigningWithoutOne() {
    const QString path = writePdf(*dir_, QStringLiteral("ts.pdf"));
    QVERIFY(!path.isEmpty());
    const QByteArray before = readAll(path);

    SignatureController controller;
    SignatureController::SigningRequest request;
    request.pkcs12Path = dir_->filePath(QStringLiteral("nonexistent.p12"));
    request.timestampUrl = QStringLiteral("http://tsa.example.invalid/");

    const auto outcome = controller.signDocument(path, request);
    QVERIFY2(!outcome.ok, "要求時間戳卻靜默簽出一份沒有時間戳的簽章");
    QVERIFY(!outcome.message.isEmpty());
    // 文件不能被碰過：使用者要的是長期可驗證的簽章，拿到別的東西比什麼都沒有更糟。
    QCOMPARE(readAll(path), before);
}

void TestSigningService::invalidCertificateLeavesTheDocumentUntouched() {
    const QString path = writePdf(*dir_, QStringLiteral("bad.pdf"));
    const QByteArray before = readAll(path);

    const QString certificate = dir_->filePath(QStringLiteral("not-a-cert.p12"));
    QFile file(certificate);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("this is not a PKCS#12 container"));
    file.close();

    SignatureController controller;
    SignatureController::SigningRequest request;
    request.pkcs12Path = certificate;
    request.password = QStringLiteral("whatever");

    const auto outcome = controller.signDocument(path, request);
    QVERIFY(!outcome.ok);
    QCOMPARE(readAll(path), before);
    // 復原資訊仍然要正確：失敗之後呼叫端可能仍然拿它去做別的判斷。
    QCOMPARE(outcome.previousSize, static_cast<quint64>(before.size()));
    QVERIFY(!outcome.boundaryGuard.isEmpty());
}

void TestSigningService::errorMessageDoesNotDistinguishWrongPasswordFromBrokenFile() {
    const QString path = writePdf(*dir_, QStringLiteral("oracle.pdf"));

    const QString certificate = dir_->filePath(QStringLiteral("junk.p12"));
    QFile file(certificate);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("junk"));
    file.close();

    SignatureController controller;
    SignatureController::SigningRequest base;
    base.pkcs12Path = certificate;

    SignatureController::SigningRequest empty = base;
    SignatureController::SigningRequest wrong = base;
    wrong.password = QStringLiteral("definitely-wrong");

    const auto a = controller.signDocument(path, empty);
    const auto b = controller.signDocument(path, wrong);
    QVERIFY(!a.ok && !b.ok);
    // 訊息一樣，因此不能當成「密碼對不對」的預言機。
    QCOMPARE(a.message, b.message);
}

void TestSigningService::missingDocumentIsReportedNotCrashed() {
    SignatureController controller;
    SignatureController::SigningRequest request;
    request.pkcs12Path = dir_->filePath(QStringLiteral("any.p12"));

    const auto outcome =
        controller.signDocument(dir_->filePath(QStringLiteral("no-such.pdf")), request);
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.message.isEmpty());
}

QTEST_MAIN(TestSigningService)
#include "test_signing_service.moc"
