// 清除所有簽章欄位（對標 PDF-XChange 的 Protect → Clear all Signatures）。
//
// 這支測試守兩件事，而第二件比第一件重要：
//
//   一、欄位真的從 /AcroForm /Fields 與頁面 /Annots 摘掉了
//   二、**簽章資料仍然留在檔案裡**
//
// 第二件不是缺陷而是增量附加的必然，但它必須被測試明確記下來。沒有這條，
// 之後很容易有人（包括我們自己）以為「清除簽章」等於「簽章資料不見了」，
// 然後把一份自以為乾淨的檔案寄出去。UI 上那段警告文字的根據就是這一條。

#include <QtTest>

#include <QByteArray>

#include "engine/signature/signature_clear.h"
#include "signature_fixture.h"

using alioth::engine::signature::clearSignatureFields;

namespace {

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// 三頁、沒有任何表單的文件。用來驗「沒有簽章欄位時什麼都不做」。
QByteArray makePlainPdf() {
    QByteArray pdf = "%PDF-1.7\n";
    const QByteArray objects[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> >>",
    };
    std::vector<int> offsets;
    for (int i = 0; i < 3; ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = static_cast<int>(pdf.size());
    pdf += "xref\n0 4\n0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) +
           "\n%%EOF\n";
    return pdf;
}

}  // namespace

class TestSignatureClear : public QObject {
    Q_OBJECT

private slots:
    void removesTheFieldAndItsWidget() {
        const QByteArray source = alioth::test::makeUnsignedSignaturePdfTemplate();
        const auto result = clearSignatureFields(toStd(source));
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.removedFields, 1);
        // 語料的欄位同時掛在 /Fields 與頁面的 /Annots 上（widget 與欄位合一，
        // 這是最常見的寫法），所以兩邊各摘掉一次。
        QCOMPARE(result.removedWidgets, 1);
        QVERIFY(result.changedAnything());

        const QByteArray after = QByteArray::fromStdString(result.bytes);
        // 增量段在後面覆寫了 catalog 與頁面，所以最後一版的 /Fields 是空的。
        // 序列化器不在鍵與自帶分隔符的值之間放空白，所以是 "/Fields[]"。
        QVERIFY2(after.lastIndexOf("/Fields[]") > after.lastIndexOf("/Fields [5 0 R]"),
                 "/Fields 沒有被改寫成空陣列");
        QVERIFY2(after.lastIndexOf("/Annots[]") > after.lastIndexOf("/Annots [5 0 R]"),
                 "頁面的 /Annots 沒有被改寫成空陣列");
        // 沒有欄位剩下時 /SigFlags 要一起拿掉：留著會讓 Acrobat 認為文件
        // 仍然含簽章欄位並據此限制操作，而使用者找不到是哪個欄位造成的。
        const int lastForm = after.lastIndexOf("/AcroForm");
        QVERIFY(lastForm >= 0);
        QVERIFY2(after.indexOf("/SigFlags", lastForm) < 0, "/SigFlags 沒有跟著拿掉");
    }

    // 增量寫入：原檔那一段逐位元組不變。這是整個物件層寫入通道的共同前提。
    void writingIsPureAppend() {
        const QByteArray source = alioth::test::makeUnsignedSignaturePdfTemplate();
        const auto result = clearSignatureFields(toStd(source));
        QVERIFY(result.ok);

        const QByteArray after = QByteArray::fromStdString(result.bytes);
        QVERIFY(after.size() > source.size());
        QCOMPARE(after.left(source.size()), source);
    }

    // **簽章資料仍然留在檔案裡。**
    //
    // 這條看起來像在測一個缺陷，其實是在釘住一個必須被說出來的事實：
    // 增量附加刪不掉東西，被移除的只是參照。UI 上那段警告文字的根據就是它。
    void signatureBytesRemainRecoverable() {
        const QByteArray source = alioth::test::makeUnsignedSignaturePdfTemplate();
        const auto result = clearSignatureFields(toStd(source));
        QVERIFY(result.ok);

        const QByteArray after = QByteArray::fromStdString(result.bytes);
        QVERIFY2(after.contains("/Type /Sig"),
                 "簽章字典竟然真的消失了——若改成全檔重寫，UI 的警告文字要一起改");
        QVERIFY2(after.contains("/ByteRange"), "/ByteRange 竟然消失了，同上");
    }

    void documentWithoutAnyFormIsLeftAlone() {
        const QByteArray source = makePlainPdf();
        const auto result = clearSignatureFields(toStd(source));
        // 成功但什麼都沒做：呼叫端據 changedAnything() 決定不寫檔——
        // 空的附加段只會讓檔案長大而使用者什麼都沒得到。
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(!result.changedAnything());
        QCOMPARE(result.removedFields, 0);
        QVERIFY(result.bytes.empty());
    }

    void brokenInputFailsLoudly() {
        const auto result = clearSignatureFields("not a pdf at all");
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.bytes.empty());
    }
};

QTEST_MAIN(TestSignatureClear)
#include "test_signature_clear.moc"
