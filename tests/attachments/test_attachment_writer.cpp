// 附件寫入（PRD-ANN-015 的寫入側）。
//
// 最重要的兩件事：加第二個附件不可以把第一個弄丟（名稱樹要合併而不是覆蓋），
// 以及輸出要通得過 qpdf 的結構檢查——名稱樹的鍵沒排序或重複，
// 用二分搜尋的解析器會找不到項目，而 PDFium 對這種錯很寬容。

#include <QtTest>

#include <QTemporaryDir>

#include "engine/attachments/attachment_reader.h"
#include "engine/attachments/attachment_writer.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"
#include "qpdf_check.h"

using namespace alioth;
using engine::attachments::AttachmentSpec;
using engine::attachments::AttachmentOrigin;

namespace {

QByteArray makeTwoPagePdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    for (int i = 0; i < 2; ++i) {
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>");
    }

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

AttachmentSpec spec(const std::string& name, const std::string& bytes) {
    AttachmentSpec value;
    value.fileName = name;
    value.bytes = bytes;
    value.mimeType = "text/plain";
    return value;
}

}  // namespace

class TestAttachmentWriter : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        base_ = makeTwoPagePdf();
    }

    void documentLevelAttachmentRoundTrips() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);

        AttachmentSpec value = spec("notes.txt", "hello attachment");
        value.description = "審閱筆記";
        const auto written = engine::attachments::addAttachment(appender, value);
        QVERIFY2(written.ok, written.diagnostic.c_str());

        const auto built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(built.bytes) == engine::objects::SourceStatus::Ok);
        const auto list = engine::attachments::listAttachments(reread);
        QCOMPARE(list.size(), std::size_t{1});
        QCOMPARE(list[0].origin, AttachmentOrigin::DocumentLevel);
        QCOMPARE(list[0].fileName, std::string{"notes.txt"});
        QCOMPARE(list[0].description, std::string{"審閱筆記"});
        QCOMPARE(list[0].mimeType, std::string{"text/plain"});
        QVERIFY(!list[0].sizeMismatch);

        const auto extracted = engine::attachments::extractAttachment(reread, list[0]);
        QVERIFY(extracted.ok);
        QCOMPARE(extracted.bytes, std::string{"hello attachment"});
    }

    void secondAttachmentDoesNotDropTheFirst() {
        // 名稱樹整棵重寫時若不合併既有項目，加第二個附件等於刪掉第一個。
        engine::objects::IncrementalAppender first;
        QVERIFY(first.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::attachments::addAttachment(first, spec("a.txt", "AAA")).ok);
        const auto once = first.build();
        QVERIFY(once.ok);

        engine::objects::IncrementalAppender second;
        QVERIFY(second.open(once.bytes) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::attachments::addAttachment(second, spec("b.txt", "BBB")).ok);
        const auto twice = second.build();
        QVERIFY(twice.ok);

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(twice.bytes) == engine::objects::SourceStatus::Ok);
        const auto list = engine::attachments::listAttachments(reread);
        QCOMPARE(list.size(), std::size_t{2});
    }

    void sameNameReplacesInsteadOfDuplicating() {
        // 名稱樹的鍵必須唯一。留兩個相同的鍵，用二分搜尋的解析器會找不到後者。
        engine::objects::IncrementalAppender first;
        QVERIFY(first.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::attachments::addAttachment(first, spec("dup.txt", "OLD")).ok);
        const auto once = first.build();

        engine::objects::IncrementalAppender second;
        QVERIFY(second.open(once.bytes) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::attachments::addAttachment(second, spec("dup.txt", "NEW")).ok);
        const auto twice = second.build();

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(twice.bytes) == engine::objects::SourceStatus::Ok);
        const auto list = engine::attachments::listAttachments(reread);
        QCOMPARE(list.size(), std::size_t{1});
        const auto extracted = engine::attachments::extractAttachment(reread, list[0]);
        QVERIFY(extracted.ok);
        QCOMPARE(extracted.bytes, std::string{"NEW"});
    }

    void fileAttachmentAnnotationLandsOnTheRightPage() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);

        AttachmentSpec value = spec("onpage.csv", "a,b,c\n");
        value.pageIndex = 1;
        value.rectPt = domain::RectF{10.0, 20.0, 30.0, 40.0};
        value.author = "Reviewer";
        const auto written = engine::attachments::addAttachment(appender, value);
        QVERIFY2(written.ok, written.diagnostic.c_str());
        QVERIFY(written.annotationObject > 0);

        const auto built = appender.build();
        QVERIFY(built.ok);

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(built.bytes) == engine::objects::SourceStatus::Ok);
        const auto list = engine::attachments::listAttachments(reread);
        QCOMPARE(list.size(), std::size_t{1});
        QCOMPARE(list[0].origin, AttachmentOrigin::FileAttachmentAnnot);
        QCOMPARE(list[0].pageIndex, 1);
        QCOMPARE(list[0].name, std::string{"Reviewer"});
    }

    void annotationIsAppendedNotReplacingExistingAnnots() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);

        AttachmentSpec a = spec("one.txt", "1");
        a.pageIndex = 0;
        AttachmentSpec b = spec("two.txt", "2");
        b.pageIndex = 0;
        QVERIFY(engine::attachments::addAttachment(appender, a).ok);
        // 第二次要讀到已經含第一則註解的頁面，否則第一則會被抹掉。
        QVERIFY(engine::attachments::addAttachment(appender, b).ok);

        const auto built = appender.build();
        QVERIFY(built.ok);
        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(built.bytes) == engine::objects::SourceStatus::Ok);
        QCOMPARE(engine::attachments::listAttachments(reread).size(), std::size_t{2});
    }

    void writeIsPurelyIncremental() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::attachments::addAttachment(appender, spec("x.txt", "X")).ok);
        const auto built = appender.build();
        QVERIFY(built.ok);
        QCOMPARE(QByteArray::fromStdString(
                     built.bytes.substr(0, static_cast<std::size_t>(base_.size()))),
                 base_);
    }

    void invalidSpecsAreRejectedWithAReason() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);

        QVERIFY(!engine::attachments::addAttachment(appender, spec("", "data")).ok);
        QVERIFY(!engine::attachments::addAttachment(appender, spec("empty.txt", "")).ok);

        AttachmentSpec offPage = spec("x.txt", "X");
        offPage.pageIndex = 99;
        const auto result = engine::attachments::addAttachment(appender, offPage);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void binaryContentSurvivesRoundTrip() {
        // 附件多半是二進位。字串裡的 0 位元組、括號、反斜線都不能讓串流長度算錯。
        std::string binary;
        for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
        binary += "()\\";

        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::attachments::addAttachment(appender, spec("bin.dat", binary)).ok);
        const auto built = appender.build();
        QVERIFY(built.ok);

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(built.bytes) == engine::objects::SourceStatus::Ok);
        const auto list = engine::attachments::listAttachments(reread);
        QCOMPARE(list.size(), std::size_t{1});
        const auto extracted = engine::attachments::extractAttachment(reread, list[0]);
        QVERIFY(extracted.ok);
        QCOMPARE(extracted.bytes, binary);
    }

    void outputPassesQpdfStructureCheck() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);
        AttachmentSpec onPage = spec("page.txt", "on page");
        onPage.pageIndex = 0;
        onPage.rectPt = domain::RectF{10.0, 10.0, 30.0, 30.0};
        QVERIFY(engine::attachments::addAttachment(appender, onPage).ok);
        QVERIFY(engine::attachments::addAttachment(appender, spec("doc.txt", "doc level")).ok);
        const auto built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = dir_->filePath(QStringLiteral("attached.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(built.bytes.data(), static_cast<qint64>(built.bytes.size()));
        file.close();

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

private:
    std::string bytes() const {
        return std::string(base_.constData(), static_cast<std::size_t>(base_.size()));
    }

    std::unique_ptr<QTemporaryDir> dir_;
    QByteArray base_;
};

QTEST_MAIN(TestAttachmentWriter)
#include "test_attachment_writer.moc"
