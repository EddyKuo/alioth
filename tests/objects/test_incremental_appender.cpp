// 增量附加器測試（ADR-002 驗收條件 2 與 5、PRD-IO-001）。
//
// 這裡驗的是「附加式寫入」這個承諾本身：原檔位元組原封不動、物件編號不相撞、
// xref 形態與原檔一致、多次存檔後 /Prev 鏈仍然完整。這些錯誤都不會讓
// PDFium 抱怨，只會讓檔案在別的閱讀器上打不開或內容莫名消失。

#include <QtTest>

#include <string>
#include <vector>

#include "engine/annotations/annotation_document.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_parser.h"
#include "object_fixture.h"
#include "qpdf_check.h"

using namespace alioth::engine::objects;
using alioth::test::makeFixturePdf;
using alioth::test::PdfFixtureOptions;
using alioth::test::toStdString;
using alioth::test::XrefKind;

namespace {

// 沿 startxref 與 /Prev 走完整條 xref 鏈，回傳每一段的位移（由新到舊）。
// 鏈斷掉的症狀是舊物件全部讀不到，而檔尾看起來完全正常。
std::vector<std::size_t> xrefChain(const std::string& bytes) {
    std::vector<std::size_t> offsets;
    const std::size_t marker = bytes.rfind("startxref");
    if (marker == std::string::npos) return offsets;

    PdfParser tail(bytes, marker + 9);
    std::string token;
    if (!tail.readKeyword(token)) return offsets;
    std::size_t offset = static_cast<std::size_t>(std::stoull(token));

    for (int guard = 0; guard < 64; ++guard) {
        if (offset >= bytes.size()) break;
        offsets.push_back(offset);

        const PdfDictionary* dict = nullptr;
        PdfObject parsed;
        if (bytes.compare(offset, 4, "xref") == 0) {
            const std::size_t trailer = bytes.find("trailer", offset);
            if (trailer == std::string::npos) break;
            PdfParser parser(bytes, trailer + 7);
            if (!parser.parseObject(parsed)) break;
        } else {
            PdfParser parser(bytes, offset);
            int number = 0;
            int generation = 0;
            if (!parser.parseIndirectObject(number, generation, parsed)) break;
        }
        dict = parsed.asDictionary();
        if (dict == nullptr) break;
        const PdfObject* previous = dict->find("Prev");
        if (previous == nullptr) break;
        offset = static_cast<std::size_t>(previous->asInteger(0));
    }
    return offsets;
}

// 用完全獨立的路徑（PDFium）確認輸出仍然是一份可讀的 PDF。
// 用我們自己的剖析器驗自己寫的位元組，等於用同一份程式碼證明自己是對的。
int pdfiumPageCount(const std::string& bytes) {
    alioth::engine::annotations::AnnotationDocument document;
    if (!document.openFromMemory(bytes.data(), bytes.size())) return -1;
    return document.pageCount();
}

std::string appendMarkerObject(const std::string& source, int* objectNumber = nullptr) {
    IncrementalAppender appender;
    if (appender.open(source) != SourceStatus::Ok) return {};
    const int number = appender.allocateObject();
    if (objectNumber != nullptr) *objectNumber = number;
    PdfDictionary dict;
    dict.set("Type", makeName("AliothMarker"));
    appender.setObject(number, PdfObject{std::move(dict)});
    const BuildResult result = appender.build();
    return result.ok ? result.bytes : std::string{};
}

}  // namespace

class TestIncrementalAppender : public QObject {
    Q_OBJECT

private slots:
    void openReadsTrailerSizeAndPages() {
        PdfFixtureOptions options;
        options.pageCount = 3;
        const std::string source = toStdString(makeFixturePdf(options));

        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        QCOMPARE(appender.source().pages().size(), std::size_t{3});
        // 物件 1..8（Catalog、Pages、3 頁 × 2），/Size 為 9。
        QCOMPARE(appender.source().trailerSize(), std::int64_t{9});
        QCOMPARE(appender.nextObjectNumber(), 9);
    }

    void allocatedNumbersNeverCollideWithExistingObjects() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        for (int i = 0; i < 5; ++i) {
            const int number = appender.allocateObject();
            QVERIFY(!appender.source().hasObject(number));
            appender.setObject(number, PdfObject{static_cast<std::int64_t>(i)});
        }
        // 不存在的物件不可以被「更新」——那等於憑空造出一個編號。
        QVERIFY(!appender.updateObject(9999, PdfObject{1}));
        QVERIFY(appender.build().ok);
    }

    void appendedFileKeepsOriginalBytesExactly() {
        PdfFixtureOptions options;
        options.pageCount = 2;
        const std::string source = toStdString(makeFixturePdf(options));
        const std::string output = appendMarkerObject(source);
        QVERIFY(!output.empty());

        // 逐位元組比對前綴。這是既有數位簽章仍然有效的唯一前提（SDD §1.3）。
        QVERIFY(output.size() > source.size());
        QCOMPARE(output.compare(0, source.size(), source), 0);
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (output[i] != source[i]) QFAIL("原檔位元組被改動");
        }
    }

    void appendedTableFileReopensInPdfium() {
        PdfFixtureOptions options;
        options.pageCount = 2;
        const std::string output = appendMarkerObject(toStdString(makeFixturePdf(options)));
        QVERIFY(!output.empty());
        QCOMPARE(pdfiumPageCount(output), 2);
    }

    void xrefStreamSourceProducesXrefStreamSection() {
        PdfFixtureOptions options;
        options.pageCount = 2;
        options.xref = XrefKind::Stream;
        const std::string source = toStdString(makeFixturePdf(options));

        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        QVERIFY(appender.source().style() == XrefStyle::Stream);
        QCOMPARE(appender.source().pages().size(), std::size_t{2});

        const std::string output = appendMarkerObject(source);
        QVERIFY(!output.empty());
        QCOMPARE(output.compare(0, source.size(), source), 0);

        // 附加段的形態必須與原檔一致：xref 串流的檔案不可以接傳統 xref 表。
        const std::size_t appendedAt = source.size();
        // 傳統 xref 表一定會寫出 trailer 關鍵字；xref 串流則不會。
        QCOMPARE(output.find("trailer", appendedAt), std::string::npos);
        QVERIFY(output.find("/Type/XRef", appendedAt) != std::string::npos);
        QCOMPARE(pdfiumPageCount(output), 2);
    }

    void xrefStreamWithPredictorIsUnderstood() {
        // /Predictor 12 是 xref 串流實務上最常見的形態。少了還原步驟，
        // 讀出來的偏移量會是一堆差值，所有物件都會指到錯的位置。
        PdfFixtureOptions options;
        options.pageCount = 2;
        options.xref = XrefKind::StreamPredictor;
        const std::string source = toStdString(makeFixturePdf(options));

        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        QCOMPARE(appender.source().pages().size(), std::size_t{2});
        const PdfObject catalog = appender.source().object(1);
        QVERIFY(catalog.asDictionary() != nullptr);
        QCOMPARE(QString::fromStdString(catalog.asDictionary()->find("Type")->asName()),
                 QStringLiteral("Catalog"));

        const std::string output = appendMarkerObject(source);
        QVERIFY(!output.empty());
        QCOMPARE(pdfiumPageCount(output), 2);
    }

    void repeatedAppendsKeepPrevChainIntact() {
        PdfFixtureOptions options;
        options.pageCount = 1;
        std::string bytes = toStdString(makeFixturePdf(options));
        const std::string original = bytes;

        std::vector<int> numbers;
        for (int round = 0; round < 3; ++round) {
            int number = 0;
            bytes = appendMarkerObject(bytes, &number);
            QVERIFY(!bytes.empty());
            numbers.push_back(number);
        }

        // 每一輪的編號都必須是新的，否則前一輪寫的東西被覆蓋。
        QCOMPARE(numbers[0] + 1, numbers[1]);
        QCOMPARE(numbers[1] + 1, numbers[2]);

        // 原檔仍在最前面，而且一個位元組都沒變。
        QCOMPARE(bytes.compare(0, original.size(), original), 0);

        // xref 鏈：三段新的加上原檔那一段。
        const std::vector<std::size_t> chain = xrefChain(bytes);
        QCOMPARE(chain.size(), std::size_t{4});
        for (std::size_t i = 1; i < chain.size(); ++i) {
            QVERIFY(chain[i] < chain[i - 1]);  // /Prev 必須指向更早的位置
        }

        // 最後仍要是一份 PDFium 開得起來的檔案，而且三個物件都讀得到。
        QCOMPARE(pdfiumPageCount(bytes), 1);
        IncrementalAppender reopened;
        QCOMPARE(reopened.open(bytes), SourceStatus::Ok);
        for (const int number : numbers) {
            const PdfObject object = reopened.source().object(number);
            QVERIFY(object.asDictionary() != nullptr);
            QCOMPARE(QString::fromStdString(object.asDictionary()->find("Type")->asName()),
                     QStringLiteral("AliothMarker"));
        }
    }

    void repeatedAppendsWorkOnXrefStreamSources() {
        PdfFixtureOptions options;
        options.xref = XrefKind::StreamPredictor;
        std::string bytes = toStdString(makeFixturePdf(options));
        for (int round = 0; round < 3; ++round) {
            bytes = appendMarkerObject(bytes);
            QVERIFY(!bytes.empty());
        }
        QCOMPARE(xrefChain(bytes).size(), std::size_t{4});
        QCOMPARE(pdfiumPageCount(bytes), 1);
    }

    void encryptedDocumentsAreRefusedExplicitly() {
        // ADR-002 驗收條件 5：字串與串流需要先加密才寫得進去，
        // 第一版明確失敗，不得寫出讀不出來的內容。
        PdfFixtureOptions options;
        options.encrypted = true;
        const std::string source = toStdString(makeFixturePdf(options));

        IncrementalAppender appender;
        std::string diagnostic;
        QCOMPARE(appender.open(source, &diagnostic), SourceStatus::Encrypted);
        QVERIFY(!diagnostic.empty());
        QVERIFY(!appender.isOpen());
        QVERIFY(!appender.build().ok);
    }

    void encryptedXrefStreamDocumentsAreAlsoRefused() {
        PdfFixtureOptions options;
        options.encrypted = true;
        options.xref = XrefKind::StreamPredictor;
        IncrementalAppender appender;
        QCOMPARE(appender.open(toStdString(makeFixturePdf(options))), SourceStatus::Encrypted);
    }

    void malformedSourcesFailFast() {
        IncrementalAppender appender;
        QCOMPARE(appender.open(""), SourceStatus::Empty);
        QCOMPARE(appender.open("not a pdf at all"), SourceStatus::NotPdf);
        QCOMPARE(appender.open("%PDF-1.7\nnothing here\n"), SourceStatus::NoStartxref);
    }

    void appendedFilesPassQpdfStructureCheck() {
        // ADR-002 驗收條件 1。qpdf 獨立於 PDFium，是唯一能自動化的第二意見；
        // 找不到工具時 skip 並說明，絕不當成通過。
        if (alioth::test::findQpdf().isEmpty()) QSKIP(alioth::test::qpdfSkipReason().constData());

        struct Case {
            const char* label;
            alioth::test::XrefKind xref;
            int rounds;
        };
        const Case cases[] = {
            {"table-single", XrefKind::Table, 1},
            {"table-repeated", XrefKind::Table, 3},
            {"xref-stream", XrefKind::Stream, 1},
            {"xref-stream-predictor-repeated", XrefKind::StreamPredictor, 3},
        };

        for (const Case& item : cases) {
            PdfFixtureOptions options;
            options.pageCount = 2;
            options.xref = item.xref;
            std::string bytes = toStdString(makeFixturePdf(options));
            for (int round = 0; round < item.rounds; ++round) {
                bytes = appendMarkerObject(bytes);
                QVERIFY(!bytes.empty());
            }
            const QString path =
                QDir::temp().filePath(QStringLiteral("alioth-objects-%1.pdf").arg(item.label));
            const alioth::test::QpdfCheckResult check =
                alioth::test::runQpdfCheckOnBytes(path, alioth::test::toByteArray(bytes));
            QVERIFY2(check.clean(),
                     alioth::test::describeQpdfFailure(QString::fromLatin1(item.label), check));
            QFile::remove(path);
        }

        // 真正會寫出去的組合：註解（含 /AP /Resources 與 /Popup）加上
        // Bates 的內容串流，兩者都動到既有的頁面物件。
        IncrementalAppender appender;
        QCOMPARE(appender.open(toStdString(makeFixturePdf())), SourceStatus::Ok);
        alioth::domain::Annotation annotation{};
        annotation.opacity = 0.4;
        annotation.geometry = alioth::domain::TextMarkupGeometry{
            alioth::domain::TextMarkupKind::Highlight,
            {alioth::domain::quadFromPageRect(alioth::domain::RectF{20, 100, 120, 116})}};
        QVERIFY(writeAnnotation(appender, 0, annotation).ok);
        ContentAppendOptions contentOptions;
        contentOptions.fonts.push_back(ContentFontRequest{"F0", "Helvetica"});
        const std::string stamp = "BT /F0 10 Tf 20 20 Td (BATES-000001) Tj ET\n";
        QVERIFY(appendPageContent(appender, 0, stamp, contentOptions).ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const QString path = QDir::temp().filePath(QStringLiteral("alioth-objects-annotated.pdf"));
        const alioth::test::QpdfCheckResult check =
            alioth::test::runQpdfCheckOnBytes(path, alioth::test::toByteArray(built.bytes));
        QVERIFY2(check.clean(),
                 alioth::test::describeQpdfFailure(QStringLiteral("annotated"), check));
        QFile::remove(path);
    }

    void emitWithoutPendingObjectsIsRefused() {
        IncrementalAppender appender;
        QCOMPARE(appender.open(toStdString(makeFixturePdf())), SourceStatus::Ok);
        const BuildResult result = appender.build();
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void incrementIsSmall() {
        // PRD-IO-001：一則變更的增量段應該遠小於原檔（驗收數字是 20 KB）。
        PdfFixtureOptions options;
        options.pageCount = 1;
        const std::string source = toStdString(makeFixturePdf(options));
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const int number = appender.allocateObject();
        appender.setObject(number, makeName("Tiny"));
        const BuildResult result = appender.build();
        QVERIFY(result.ok);
        QVERIFY(result.appendedBytes < 20u * 1024u);
        QCOMPARE(result.bytes.size(), source.size() + result.appendedBytes);
    }
};

QTEST_APPLESS_MAIN(TestIncrementalAppender)
#include "test_incremental_appender.moc"
