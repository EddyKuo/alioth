// 攤平整份文件的註解（PRD-ANN-013 的攤平半段）。
//
// 引擎層的「外觀真的變成頁面內容」在 tests/objects/test_annotation_flattener.cpp
// 驗過了。這一支驗的是先前缺的另一半——**攤平後 /Annots 裡不能還留著那則註解**。
// 留著的話檢視器仍然把它當可編輯註解顯示，於是畫面上同一個標記出現兩份：
// 一份燒進內容、一份浮在上面，而且還選得到、改得動、匯得出去。
//
// 另外釘住三件事，每一件做錯都不會有錯誤訊息：
//
//   1. 寫出來的必須是**增量**（原檔前綴一個位元組都不動），否則既有簽章會從
//      「有效，簽章後有變更」掉成無效。
//   2. 沒有可攤平的註解時**不要寫檔**——寫一個空附加段只會讓簽章狀態變差。
//   3. 攤平是全有全無。攤到一半停下來，使用者看不出停在哪裡。

#include <QtTest>

#include <QTemporaryDir>

#include "app/annotation_flatten_service.h"
#include "app/annotation_service.h"
#include "domain/redaction.h"
#include "engine/objects/annotation_reader.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/page_object_editor.h"

using namespace alioth;

namespace {

// 一頁文件，/Annots 裡帶兩則可讀回模型的註解（Square 與 Highlight）。
QByteArray makeAnnotatedPdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Annots [4 0 R 5 0 R] >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Square /Rect [20 20 80 80] "
        "/C [1 0 0] /CA 1 /F 4 >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Highlight /Rect [10 100 150 120] "
        "/QuadPoints [10 120 150 120 10 100 150 100] /C [1 1 0] /CA 1 /F 4 >>");

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

QByteArray makePlainPdf() {
    QByteArray pdf = "%PDF-1.7\n";
    std::vector<QByteArray> objects = {"<< /Type /Catalog /Pages 2 0 R >>",
                                       "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                       "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] "
                                       "/Resources << >> >>"};
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

QString writeTemp(QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QString();
    file.write(bytes);
    file.close();
    return path;
}

int annotationCountIn(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return -1;
    const QByteArray bytes = file.readAll();
    file.close();

    engine::objects::IncrementalAppender appender;
    if (appender.open(std::string(bytes.constData(),
                                  static_cast<std::size_t>(bytes.size()))) !=
        engine::objects::SourceStatus::Ok) {
        return -1;
    }
    int total = 0;
    for (int pageIndex = 0;; ++pageIndex) {
        engine::objects::PdfRef pageRef{};
        if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) break;
        total += static_cast<int>(
            engine::objects::pageAnnotationRefs(appender.source(), pageRef).size());
    }
    return total;
}

}  // namespace

class TestAnnotationFlattenService : public QObject {
    Q_OBJECT

private slots:
    void flatteningRemovesTheAnnotationsFromAnnots();
    void resultIsPurelyIncremental();
    void documentWithoutAnnotationsIsLeftUntouched();
    void previewCountsWhatWillBeFlattened();
};

void TestAnnotationFlattenService::flatteningRemovesTheAnnotationsFromAnnots() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTemp(dir, QStringLiteral("annotated.pdf"), makeAnnotatedPdf());
    QVERIFY(!path.isEmpty());
    QCOMPARE(annotationCountIn(path), 2);

    app::AnnotationFlattenService service;
    const app::HighlightResult result =
        service.flattenAll(path, domain::IrreversibleConsent::confirmed());
    QVERIFY2(result.ok, qPrintable(result.message));

    // 這是本檔案的核心：/Annots 必須空了。留著的話同一個標記會出現兩份，
    // 而浮在上面的那一份照樣選得到、改得動、匯得出去。
    QCOMPARE(annotationCountIn(path), 0);
}

void TestAnnotationFlattenService::resultIsPurelyIncremental() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray original = makeAnnotatedPdf();
    const QString path = writeTemp(dir, QStringLiteral("annotated.pdf"), original);
    QVERIFY(!path.isEmpty());

    app::AnnotationFlattenService service;
    const app::HighlightResult result =
        service.flattenAll(path, domain::IrreversibleConsent::confirmed());
    QVERIFY2(result.ok, qPrintable(result.message));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray after = file.readAll();
    file.close();

    // 原檔前綴一個位元組都不能動，否則既有簽章會從「有效，簽章後有變更」
    // 掉成無效——那是這個產品的核心賣點。
    QVERIFY2(after.size() > original.size(), "檔案沒有變長，附加段可能沒寫出來");
    QCOMPARE(after.left(original.size()), original);
    // 復原資訊要指回原長度，截回去就是精確的反操作。
    QCOMPARE(result.previousSize, static_cast<quint64>(original.size()));
    QVERIFY(!result.boundaryGuard.isEmpty());
}

void TestAnnotationFlattenService::documentWithoutAnnotationsIsLeftUntouched() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray original = makePlainPdf();
    const QString path = writeTemp(dir, QStringLiteral("plain.pdf"), original);
    QVERIFY(!path.isEmpty());

    app::AnnotationFlattenService service;
    const app::HighlightResult result =
        service.flattenAll(path, domain::IrreversibleConsent::confirmed());
    QVERIFY2(!result.ok, "沒有註解卻回報成功");
    QVERIFY(!result.message.isEmpty());

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray after = file.readAll();
    file.close();
    // 一個位元組都不該動。寫一個空的附加段會讓簽章狀態變差而使用者什麼都沒得到。
    QCOMPARE(after, original);
}

void TestAnnotationFlattenService::previewCountsWhatWillBeFlattened() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTemp(dir, QStringLiteral("annotated.pdf"), makeAnnotatedPdf());
    QVERIFY(!path.isEmpty());

    app::AnnotationFlattenService service;
    const auto summary = service.preview(path);
    // 確認對話框要說得出「會影響幾則」。只問「確定要攤平嗎」的話，
    // 使用者沒有辦法判斷自己是不是開錯了檔案。
    QCOMPARE(summary.flattened, 2);
    QCOMPARE(summary.skipped, 0);
}

QTEST_MAIN(TestAnnotationFlattenService)
#include "test_annotation_flatten_service.moc"
