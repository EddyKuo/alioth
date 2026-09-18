// 空間使用稽核（對標 PDF-XChange 的 File → Audit Space Usage）。
//
// 這份報告唯一的用途是回答「這份檔案是什麼占掉的」，而使用者問這個問題時
// 正打算把它寄出去。因此測試的重點不是數字精確到位元組，而是**分類不能錯**：
// 把影像算進「其他」會讓使用者去重壓一個根本不大的東西，而真正該處理的
// 那一項他永遠看不到。

#include <QtTest>

#include <QByteArray>

#include "engine/objects/space_audit.h"

using alioth::engine::objects::auditSpaceUsage;
using alioth::engine::objects::SpaceCategory;

namespace {

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// 一份有影像、字型、內容串流與註解的單頁文件。每一類都放得夠大，
// 讓「分錯類」在數字上看得出來。
QByteArray makeMixedPdf() {
    const QByteArray imageData(4096, 'I');
    const QByteArray fontData(2048, 'F');
    const QByteArray contentData = "BT /F0 12 Tf 72 720 Td (hello) Tj ET\n" + QByteArray(1024, ' ');

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R "
        "/Resources << /XObject << /Im0 5 0 R >> /Font << /F0 6 0 R >> >> /Annots [8 0 R] >>");
    objects.push_back("<< /Length " + QByteArray::number(contentData.size()) + " >>\nstream\n" +
                      contentData + "\nendstream");
    objects.push_back("<< /Type /XObject /Subtype /Image /Width 32 /Height 32 "
                      "/ColorSpace /DeviceGray /BitsPerComponent 8 /Length " +
                      QByteArray::number(imageData.size()) + " >>\nstream\n" + imageData +
                      "\nendstream");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FontDescriptor 7 0 R >>");
    objects.push_back("<< /Type /FontDescriptor /FontName /Helvetica /FontFile 9 0 R >>");
    objects.push_back("<< /Type /Annot /Subtype /Square /Rect [10 10 100 100] /F 4 >>");
    objects.push_back("<< /Length1 " + QByteArray::number(fontData.size()) + " /Length " +
                      QByteArray::number(fontData.size()) + " >>\nstream\n" + fontData +
                      "\nendstream");

    QByteArray pdf = "%PDF-1.7\n";
    std::vector<int> offsets;
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[static_cast<std::size_t>(i)] +
               "\nendobj\n";
    }
    const int xref = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           "\n0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

std::uint64_t bytesOf(const alioth::engine::objects::SpaceAuditResult& audit,
                      SpaceCategory category) {
    for (const auto& usage : audit.categories) {
        if (usage.category == category) return usage.bytes;
    }
    return 0;
}

}  // namespace

class TestSpaceAudit : public QObject {
    Q_OBJECT

private slots:
    void classifiesEachKindOfObject() {
        const auto audit = auditSpaceUsage(toStd(makeMixedPdf()));
        QVERIFY2(audit.ok, audit.diagnostic.c_str());

        // 影像是這份語料裡最大的一項（4 KB），所以它必須排第一——
        // 使用者只會對排第一的那一項採取行動。
        QVERIFY(!audit.categories.empty());
        QCOMPARE(audit.categories.front().category, SpaceCategory::Images);

        QVERIFY2(bytesOf(audit, SpaceCategory::Images) > 4000, "影像沒有被認出來");
        QVERIFY2(bytesOf(audit, SpaceCategory::Fonts) > 2000,
                 "字型程式串流沒有被認出來（靠 /Length1 辨識）");
        QVERIFY2(bytesOf(audit, SpaceCategory::ContentStreams) > 1000,
                 "頁面內容串流沒有被認出來——它沒有 /Type，必須從 /Contents 反查");
        QVERIFY2(bytesOf(audit, SpaceCategory::Annotations) > 0, "註解沒有被認出來");
        QVERIFY2(bytesOf(audit, SpaceCategory::Structure) > 0, "catalog 與頁面樹沒有被認出來");
    }

    // 各類別加起來要等於 objectBytes，而 objectBytes 不會超過檔案大小。
    // 兩者任一不成立，報告裡的百分比就是錯的。
    void totalsAreConsistent() {
        const QByteArray pdf = makeMixedPdf();
        const auto audit = auditSpaceUsage(toStd(pdf));
        QVERIFY(audit.ok);

        std::uint64_t sum = 0;
        for (const auto& usage : audit.categories) sum += usage.bytes;
        QCOMPARE(sum, audit.objectBytes);

        QCOMPARE(audit.fileBytes, static_cast<std::uint64_t>(pdf.size()));
        QVERIFY2(audit.objectBytes <= audit.fileBytes,
                 "物件總長超過檔案大小，百分比會算出大於 100%");
        QCOMPARE(audit.overheadBytes(), audit.fileBytes - audit.objectBytes);
    }

    void brokenInputFailsLoudly() {
        const auto audit = auditSpaceUsage("definitely not a pdf");
        QVERIFY(!audit.ok);
        QVERIFY(!audit.diagnostic.empty());
        QVERIFY(audit.categories.empty());
    }
};

QTEST_MAIN(TestSpaceAudit)
#include "test_space_audit.moc"
