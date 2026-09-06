// 附件讀取（PRD-ANN-015）。
//
// 重點有三：兩種附件都要列出、取不出內容時要明確失敗而不是給出垃圾、
// 以及檔名清理——附件檔名是不可信輸入，直接拿去開檔就是路徑穿越。

#include <QtTest>

#include "engine/attachments/attachment_reader.h"
#include "engine/objects/pdf_source_document.h"

using namespace alioth;
using engine::attachments::Attachment;
using engine::attachments::AttachmentOrigin;

namespace {

QByteArray streamObject(const QByteArray& extras, const QByteArray& data) {
    return "<< " + extras + " /Length " + QByteArray::number(data.size()) + " >>\nstream\n" + data +
           "\nendstream";
}

// 一頁文件，帶：
//   文件層附件 "spec.txt"（未壓縮，/Size 正確）
//   文件層附件 "wrong-size.txt"（/Size 刻意寫錯）
//   文件層附件 "lzw.bin"（宣稱 LZWDecode，我們不支援）
//   頁面上的檔案附件註解 "note.csv"
QByteArray makeAttachmentPdf() {
    std::vector<QByteArray> objects;

    objects.push_back("<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 4 0 R >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> /Annots [12 0 R] >>");

    // 4：EmbeddedFiles 名稱樹
    objects.push_back(
        "<< /Names [ (lzw.bin) 9 0 R (spec.txt) 5 0 R (wrong-size.txt) 7 0 R ] >>");

    // 5/6：spec.txt
    objects.push_back(
        "<< /Type /Filespec /F (spec.txt) /UF (spec.txt) /Desc (\xe8\xa6\x8f\xe7\xaf\x84) "
        "/EF << /F 6 0 R >> >>");
    const QByteArray specData = "line one\nline two\n";
    objects.push_back(streamObject(
        "/Type /EmbeddedFile /Subtype /text#2Fplain /Params << /Size " +
            QByteArray::number(specData.size()) +
            " /CreationDate (D:20260101120000+08'00') >>",
        specData));

    // 7/8：wrong-size.txt
    objects.push_back("<< /Type /Filespec /F (wrong-size.txt) /EF << /F 8 0 R >> >>");
    objects.push_back(
        streamObject("/Type /EmbeddedFile /Params << /Size 999999 >>", "short"));

    // 9/10：lzw.bin
    objects.push_back("<< /Type /Filespec /F (lzw.bin) /EF << /F 10 0 R >> >>");
    objects.push_back(streamObject("/Type /EmbeddedFile /Filter /LZWDecode", "\x01\x02\x03"));

    // 11：註解用的 filespec；12：檔案附件註解
    objects.push_back("<< /Type /Filespec /F (note.csv) /EF << /F 6 0 R >> >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /FileAttachment /Rect [10 10 30 30] /T (Bob) "
        "/Contents (\xe9\x99\x84\xe4\xbb\xb6\xe8\xaa\xaa\xe6\x98\x8e) /FS 11 0 R >>");

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

const Attachment* find(const std::vector<Attachment>& list, const std::string& fileName) {
    for (const Attachment& item : list) {
        if (item.fileName == fileName) return &item;
    }
    return nullptr;
}

}  // namespace

class TestAttachmentReader : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        const QByteArray pdf = makeAttachmentPdf();
        QVERIFY(source_.open(std::string(pdf.constData(),
                                         static_cast<std::size_t>(pdf.size()))) ==
                engine::objects::SourceStatus::Ok);
        list_ = engine::attachments::listAttachments(source_);
    }

    void bothKindsOfAttachmentAreListed() {
        // 只列其中一種，使用者會在另一種存在時以為文件沒有附件。
        QCOMPARE(list_.size(), std::size_t{4});

        int documentLevel = 0;
        int annotations = 0;
        for (const Attachment& item : list_) {
            if (item.origin == AttachmentOrigin::DocumentLevel) ++documentLevel;
            if (item.origin == AttachmentOrigin::FileAttachmentAnnot) ++annotations;
        }
        QCOMPARE(documentLevel, 3);
        QCOMPARE(annotations, 1);
    }

    void metadataIsRead() {
        const Attachment* spec = find(list_, "spec.txt");
        QVERIFY(spec != nullptr);
        QCOMPARE(spec->name, std::string{"spec.txt"});
        QCOMPARE(spec->description, std::string{"規範"});
        // /Subtype 的斜線在 PDF 裡以 #2F 跳脫，解析器要還原它。
        QCOMPARE(spec->mimeType, std::string{"text/plain"});
        QCOMPARE(spec->size, static_cast<std::int64_t>(std::string("line one\nline two\n").size()));
        QCOMPARE(spec->creationDate, std::string{"D:20260101120000+08'00'"});
        QVERIFY(!spec->sizeMismatch);
    }

    void annotationAttachmentCarriesItsPage() {
        const Attachment* note = find(list_, "note.csv");
        QVERIFY(note != nullptr);
        QCOMPARE(note->origin, AttachmentOrigin::FileAttachmentAnnot);
        QCOMPARE(note->pageIndex, 0);
        QCOMPARE(note->name, std::string{"Bob"});
        QCOMPARE(note->description, std::string{"附件說明"});
    }

    void extractReturnsTheDecodedBytes() {
        const Attachment* spec = find(list_, "spec.txt");
        QVERIFY(spec != nullptr);
        const auto result = engine::attachments::extractAttachment(source_, *spec);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.bytes, std::string{"line one\nline two\n"});
    }

    void declaredSizeMismatchIsFlagged() {
        // 照著 /Size 顯示「1 MB」但只取得出 5 個位元組，使用者會以為是我們壞了。
        const Attachment* wrong = find(list_, "wrong-size.txt");
        QVERIFY(wrong != nullptr);
        QVERIFY(wrong->sizeMismatch);
    }

    void unsupportedFilterFailsLoudly() {
        // 把沒解開的壓縮位元組寫成檔案，使用者拿到的是垃圾，而且他無從得知。
        const Attachment* lzw = find(list_, "lzw.bin");
        QVERIFY(lzw != nullptr);
        const auto result = engine::attachments::extractAttachment(source_, *lzw);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.bytes.empty());
    }

    void attachmentWithoutStreamFailsCleanly() {
        Attachment broken;
        broken.streamObject = 0;
        const auto result = engine::attachments::extractAttachment(source_, broken);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void fileNameSanitizationBlocksPathTraversal() {
        using engine::attachments::sanitizeAttachmentFileName;
        // 分隔符號被拿掉，開頭的句點也被前後修剪吃掉，結果是一個不可能
        // 指向別的目錄的單純檔名。
        QCOMPARE(sanitizeAttachmentFileName("../../Windows/System32/evil.dll"),
                 std::string{"WindowsSystem32evil.dll"});
        QCOMPARE(sanitizeAttachmentFileName("..\\..\\evil.exe"), std::string{"evil.exe"});
        QVERIFY(sanitizeAttachmentFileName("a/b").find('/') == std::string::npos);
    }

    void fileNameSanitizationHandlesWindowsQuirks() {
        using engine::attachments::sanitizeAttachmentFileName;
        // Windows 會吃掉結尾的句點與空白，"evil.exe." 存下來就是 "evil.exe"。
        QCOMPARE(sanitizeAttachmentFileName("evil.exe. "), std::string{"evil.exe"});
        // 保留裝置名稱：用它當檔名會開到裝置而不是檔案。
        QCOMPARE(sanitizeAttachmentFileName("CON"), std::string{"_CON"});
        QCOMPARE(sanitizeAttachmentFileName("con.txt"), std::string{"_con.txt"});
        QCOMPARE(sanitizeAttachmentFileName("console.txt"), std::string{"console.txt"});
        // 控制字元一律移除。
        QCOMPARE(sanitizeAttachmentFileName(std::string("a\x01\x02" "b.txt")),
                 std::string{"ab.txt"});
    }

    void unusableFileNameYieldsEmptyString() {
        // 猜一個名字給使用者，反而更難察覺出了什麼事。
        using engine::attachments::sanitizeAttachmentFileName;
        QVERIFY(sanitizeAttachmentFileName("///").empty());
        QVERIFY(sanitizeAttachmentFileName("   ").empty());
        QVERIFY(sanitizeAttachmentFileName("").empty());
    }

    void documentWithoutAttachmentsIsNotAnError() {
        const QByteArray plain =
            "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
            "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
            "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>\n"
            "endobj\ntrailer\n<< /Size 4 /Root 1 0 R >>\n%%EOF\n";
        engine::objects::PdfSourceDocument source;
        // 這份沒有 xref，解析可能失敗；失敗時本測試不成立，跳過即可。
        if (source.open(std::string(plain.constData(),
                                    static_cast<std::size_t>(plain.size()))) !=
            engine::objects::SourceStatus::Ok) {
            QSKIP("語料無法解析，本例不適用");
        }
        QVERIFY(engine::attachments::listAttachments(source).empty());
    }

private:
    engine::objects::PdfSourceDocument source_;
    std::vector<Attachment> list_;
};

QTEST_MAIN(TestAttachmentReader)
#include "test_attachment_reader.moc"
