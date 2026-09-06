// 密碼與權限的測試（WP27，PRD-SEC-002）。
//
// 三段可行性分開測：
//   一、讀出加密演算法——純粹解析 /Encrypt 字典，不需要密碼也不需要真的解密。
//   二、移除密碼——用一份真的通過標準安全處理器驗證演算法的語料
//      （encrypted_fixture.h），證明 PDFium 真的能打開、驗出受限的權限旗標，
//      而 removePassword 之後那些限制真的消失。
//   三、設定密碼——目前唯一合法的行為是回報不支援，測的是這個回報本身
//      而不是一個不存在的功能。

#include <QtTest>

#include <QFileInfo>
#include <QTemporaryDir>

#include "encrypted_fixture.h"
#include "engine/save/security_saver.h"
#include "save/save_fixture.h"

using namespace alioth::engine::save;
using alioth::test::makeBulkyPdf;
using alioth::test::readAll;
using alioth::test::writePdfTo;

namespace {

// 只為了測 /Encrypt 字典解析而造的最小語料：字典本身不必是真的能被 PDFium
// 解密的內容（那件事已經由 encrypted_fixture.h 的 RC4 語料證明過），
// 這裡只驗證 inspectEncryption() 對不同 /V /R /CF 組合的判讀邏輯。
QByteArray makeDictOnlyEncryptedPdf(const QByteArray& encryptDictBody) {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] "
                      "/Resources << >> >>");
    objects.push_back(encryptDictBody);

    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R /Encrypt 4 0 R >>\nstartxref\n" + QByteArray::number(xrefOffset) +
           "\n%%EOF\n";
    return pdf;
}

}  // namespace

class TestSecuritySaver : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void cleanup() { dir_.reset(); }

    void unencryptedDocumentReportsNone() {
        const QByteArray pdf = makeBulkyPdf(1, 2000);
        const SecurityInfo info =
            inspectEncryption(std::string(pdf.constData(), static_cast<std::size_t>(pdf.size())));
        QVERIFY(!info.encrypted);
        QCOMPARE(info.algorithm, EncryptionAlgorithm::None);
        QVERIFY(info.algorithmKnown);
    }

    void rc4_40BitIsRecognized() {
        const QByteArray pdf =
            makeDictOnlyEncryptedPdf("<< /Filter /Standard /V 1 /R 2 /O <00> /U <00> /P -1 >>");
        const SecurityInfo info =
            inspectEncryption(std::string(pdf.constData(), static_cast<std::size_t>(pdf.size())));
        QVERIFY(info.encrypted);
        QVERIFY(info.algorithmKnown);
        QCOMPARE(info.algorithm, EncryptionAlgorithm::Rc4_40);
    }

    void rc4_128BitIsRecognizedByLength() {
        const QByteArray pdf = makeDictOnlyEncryptedPdf(
            "<< /Filter /Standard /V 2 /R 3 /Length 128 /O <00> /U <00> /P -1 >>");
        const SecurityInfo info =
            inspectEncryption(std::string(pdf.constData(), static_cast<std::size_t>(pdf.size())));
        QVERIFY(info.algorithmKnown);
        QCOMPARE(info.algorithm, EncryptionAlgorithm::Rc4_128);
    }

    void aes128IsRecognizedViaCryptFilter() {
        const QByteArray pdf = makeDictOnlyEncryptedPdf(
            "<< /Filter /Standard /V 4 /R 4 /O <00> /U <00> /P -1 "
            "/CF << /StdCF << /CFM /AESV2 >> >> /StmF /StdCF /StrF /StdCF >>");
        const SecurityInfo info =
            inspectEncryption(std::string(pdf.constData(), static_cast<std::size_t>(pdf.size())));
        QVERIFY(info.algorithmKnown);
        QCOMPARE(info.algorithm, EncryptionAlgorithm::Aes128);
    }

    void aes256IsRecognizedViaCryptFilter() {
        const QByteArray pdf = makeDictOnlyEncryptedPdf(
            "<< /Filter /Standard /V 5 /R 6 /O <00> /U <00> /P -1 "
            "/CF << /StdCF << /CFM /AESV3 >> >> /StmF /StdCF /StrF /StdCF >>");
        const SecurityInfo info =
            inspectEncryption(std::string(pdf.constData(), static_cast<std::size_t>(pdf.size())));
        QVERIFY(info.algorithmKnown);
        QCOMPARE(info.algorithm, EncryptionAlgorithm::Aes256);
    }

    // 核心的一支：真的能被 PDFium 解鎖的加密文件，權限旗標受限；
    // removePassword 之後，這份限制必須真的消失，而不是只是我們自己回報消失。
    void removePasswordUnlocksRestrictedDocument() {
        // 只清掉「列印」這一位元，其餘全部維持允許，這樣才驗得出「只有列印
        // 被鎖住」是解碼邏輯做對了，而不是巧合地全部都是 false。
        const std::int32_t permissions = static_cast<std::int32_t>(0xFFFFFFFBu);
        const alioth::test::EncryptedPdfSpec spec =
            alioth::test::makeRc4EncryptedPdf(/*userPassword=*/"", /*ownerPassword=*/"OwnerSecret",
                                              permissions);

        const QString path = dir_->filePath(QStringLiteral("protected.pdf"));
        QVERIFY(writePdfTo(path, spec.bytes));

        ScopedDocument doc;
        QVERIFY2(doc.open(path.toStdString()),
                 "無法用空使用者密碼開啟受保護文件——加密語料的金鑰推導很可能算錯了");

        SecurityInfo info =
            inspectEncryption(std::string(spec.bytes.constData(),
                                          static_cast<std::size_t>(spec.bytes.size())));
        QVERIFY(info.encrypted);
        QCOMPARE(info.algorithm, EncryptionAlgorithm::Rc4_40);
        fillPermissions(doc.handle(), info);
        QVERIFY2(!info.permissions.print, "應該被限制列印，卻讀到允許");
        QVERIFY(info.permissions.modify);
        QVERIFY(info.permissions.copy);

        const QString target = dir_->filePath(QStringLiteral("unlocked.pdf"));
        const SaveResult result = removePassword(doc.handle(), target.toStdString());
        QVERIFY2(result.ok(), result.message.c_str());

        SecurityInfo afterInfo;
        const QByteArray unlockedBytes = readAll(target);
        QVERIFY(!unlockedBytes.isEmpty());
        afterInfo = inspectEncryption(std::string(
            unlockedBytes.constData(), static_cast<std::size_t>(unlockedBytes.size())));
        QVERIFY2(!afterInfo.encrypted, "removePassword 之後 /Encrypt 仍然存在");

        // 用另一份獨立把手重新開啟，確認不需要密碼、權限也真的解除了——
        // 只看 /Encrypt 有沒有消失不夠，萬一權限旗標被某處快取住而不是真的解密。
        ScopedDocument reopened;
        QVERIFY(reopened.open(target.toStdString()));
        SecurityInfo reopenedInfo;
        fillPermissions(reopened.handle(), reopenedInfo);
        QVERIFY(reopenedInfo.permissions.print);
    }

    void setPasswordIsExplicitlyUnsupported() {
        const SetPasswordResult result = setPassword("new-user", "new-owner", 0xFFFFFFFC);
        QCOMPARE(result.status, SetPasswordStatus::NotSupported);
        QVERIFY(!result.ok());
        QVERIFY2(!result.message.empty(), "不支援的功能也必須說明原因，不能只回傳失敗");
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_APPLESS_MAIN(TestSecuritySaver)
#include "test_security_saver.moc"
