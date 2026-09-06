// 註解寫入器測試（WBS 4.1 的 PDFium 端）。
//
// 上一支測試證明我們產生的位元組是對的，這支證明它們真的進了檔案而且讀得回來。
// PDFium 不會自動替我們補 /AP，所以「寫進去之後 /AP 還在」這件事必須逐次驗證，
// 否則整個外觀串流工作包等於沒有出海口。

#include <QtTest>

#include <string>

#include "content_stream_check.h"
#include "domain/annotation.h"
#include "engine/annotations/annotation_document.h"
#include "engine/annotations/appearance_stream.h"
#include "pdf_fixture.h"

using namespace alioth::domain;
using namespace alioth::engine::annotations;
using alioth::test::checkContentStream;
using alioth::test::ContentStreamReport;
using alioth::test::makeSinglePagePdf;

namespace {

Annotation makeTwoLineHighlight() {
    Annotation annotation{};
    annotation.id = "alioth-0001";
    annotation.color = ColorRgb{1.0, 0.9, 0.1};
    annotation.opacity = 0.4;
    annotation.author = "審閱者";
    annotation.contents = "這段需要確認";
    annotation.subject = "螢光筆";
    annotation.creationDate = PdfDate{2026, 9, 5, 14, 30, 0, 8, 0};
    annotation.modifiedDate = PdfDate{2026, 9, 5, 15, 0, 0, 8, 0};
    annotation.flags = AnnotationFlag::Print;
    annotation.geometry = TextMarkupGeometry{
        TextMarkupKind::Highlight,
        {quadFromPageRect(RectF{20.0, 300.0, 180.0, 320.0}),
         quadFromPageRect(RectF{20.0, 270.0, 140.0, 290.0})}};
    return annotation;
}

}  // namespace

class TestAnnotationWriter : public QObject {
    Q_OBJECT

private slots:
    void writesHighlightWithAppearanceStream() {
        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));
        QCOMPARE(document.pageCount(), 1);
        QCOMPARE(document.annotationCount(0), 0);

        const Annotation annotation = makeTwoLineHighlight();
        const WriteResult result = document.addAnnotation(0, annotation);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.index, 0);
        QVERIFY(result.appearanceWritten);
        // PDFium 的 SetAP 不建 /Resources，Multiply 因此進不了外觀串流。
        // 這個降級必須被回報出來，不能是沉默的行為差異。
        QVERIFY(result.blendModeElided);

        QCOMPARE(document.annotationCount(0), 1);
        const auto subtype = document.subtypeName(0, 0);
        QVERIFY(subtype.has_value());
        QCOMPARE(QString::fromStdString(*subtype), QStringLiteral("Highlight"));
        QCOMPARE(document.quadPointCount(0, 0), std::size_t{2});
    }

    // 這是本工作包存在的理由：PDFium 對多數註解型別不會自動產生 /AP。
    void appearanceStreamSurvivesRoundTrip() {
        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));

        const Annotation annotation = makeTwoLineHighlight();
        const WriteResult result = document.addAnnotation(0, annotation);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const auto stored = document.appearanceStream(0, 0);
        QVERIFY(stored.has_value());
        QVERIFY(!stored->empty());

        AppearanceOptions options{};
        options.resourcesSupported = false;
        const Appearance expected = generateAppearance(annotation, options);
        QVERIFY(expected.valid);
        QCOMPARE(QString::fromStdString(*stored), QString::fromStdString(expected.content));

        const ContentStreamReport report =
            checkContentStream(QByteArray::fromStdString(*stored));
        QVERIFY2(report.valid, qPrintable(report.error));
        QCOMPARE(report.countOperator(QStringLiteral("h")), 2);
        QCOMPARE(report.countOperator(QStringLiteral("f")), 1);
    }

    // /Rect 取外觀串流算出的實際塗佈範圍，不是呼叫端隨手給的框。
    void rectangleMatchesAppearanceBoundingBox() {
        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));

        Annotation annotation{};
        annotation.rect = RectF{10.0, 10.0, 110.0, 60.0};
        annotation.color = ColorRgb{1.0, 0.0, 0.0};
        annotation.border.width = 3.0;
        annotation.geometry = ShapeGeometry{ShapeKind::Square};

        const WriteResult result = document.addAnnotation(0, annotation);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const auto rect = document.annotationRect(0, 0);
        QVERIFY(rect.has_value());
        QCOMPARE(rect->left, 10.0);
        QCOMPARE(rect->bottom, 10.0);
        QCOMPARE(rect->right, 110.0);
        QCOMPARE(rect->top, 60.0);
    }

    void writesStandardDictionaryKeys() {
        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));

        const Annotation annotation = makeTwoLineHighlight();
        const WriteResult result = document.addAnnotation(0, annotation);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const auto author = document.stringValue(0, 0, "T");
        QVERIFY(author.has_value());
        QCOMPARE(QString::fromStdString(*author), QStringLiteral("審閱者"));

        const auto contents = document.stringValue(0, 0, "Contents");
        QVERIFY(contents.has_value());
        QCOMPARE(QString::fromStdString(*contents), QStringLiteral("這段需要確認"));

        const auto name = document.stringValue(0, 0, "NM");
        QVERIFY(name.has_value());
        QCOMPARE(QString::fromStdString(*name), QStringLiteral("alioth-0001"));

        const auto created = document.stringValue(0, 0, "CreationDate");
        QVERIFY(created.has_value());
        QCOMPARE(QString::fromStdString(*created), QStringLiteral("D:20260905143000+08'00'"));

        const auto modified = document.stringValue(0, 0, "M");
        QVERIFY(modified.has_value());
        QCOMPARE(QString::fromStdString(*modified), QStringLiteral("D:20260905150000+08'00'"));

        const auto flags = document.annotationFlags(0, 0);
        QVERIFY(flags.has_value());
        QCOMPARE(*flags, static_cast<int>(AnnotationFlag::Print));
    }

    // 存檔後重新開啟仍要看得到 /AP。只在記憶體中正確是不夠的——
    // 序列化路徑上任何一步吃掉 /AP，在別的檢視器上就是註解消失。
    void appearanceSurvivesIncrementalSave() {
        const QByteArray pdf = makeSinglePagePdf();
        std::vector<unsigned char> saved;
        std::string expectedContent;
        {
            AnnotationDocument document;
            QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));
            const Annotation annotation = makeTwoLineHighlight();
            const WriteResult result = document.addAnnotation(0, annotation);
            QVERIFY2(result.ok, result.diagnostic.c_str());
            expectedContent = *document.appearanceStream(0, 0);
            saved = document.saveIncremental();
        }
        QVERIFY(!saved.empty());
        // 增量儲存：原始位元組原封不動地留在檔頭，簽章才不會被判定為無效。
        QVERIFY(saved.size() > static_cast<std::size_t>(pdf.size()));
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(saved.data()), pdf.size()), pdf);

        AnnotationDocument reopened;
        QVERIFY(reopened.openFromMemory(saved.data(), saved.size()));
        QCOMPARE(reopened.pageCount(), 1);
        QCOMPARE(reopened.annotationCount(0), 1);

        const auto ap = reopened.appearanceStream(0, 0);
        QVERIFY(ap.has_value());
        QCOMPARE(QString::fromStdString(*ap), QString::fromStdString(expectedContent));
        QCOMPARE(reopened.quadPointCount(0, 0), std::size_t{2});
    }

    void writesUnderlineStrikeOutInkAndNote_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("expectedSubtype");
        QTest::newRow("underline") << 0 << QStringLiteral("Underline");
        QTest::newRow("strikeout") << 1 << QStringLiteral("StrikeOut");
        QTest::newRow("ink") << 2 << QStringLiteral("Ink");
        QTest::newRow("note") << 3 << QStringLiteral("Text");
        QTest::newRow("circle") << 4 << QStringLiteral("Circle");
    }

    void writesUnderlineStrikeOutInkAndNote() {
        QFETCH(int, kind);
        QFETCH(QString, expectedSubtype);

        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));

        Annotation annotation{};
        annotation.rect = RectF{20.0, 200.0, 120.0, 260.0};
        annotation.color = ColorRgb{0.0, 0.4, 0.9};
        annotation.border.width = 2.0;
        const std::vector<QuadPoint> quads{quadFromPageRect(RectF{20.0, 200.0, 120.0, 220.0})};
        switch (kind) {
            case 0: annotation.geometry = TextMarkupGeometry{TextMarkupKind::Underline, quads}; break;
            case 1: annotation.geometry = TextMarkupGeometry{TextMarkupKind::StrikeOut, quads}; break;
            case 2:
                annotation.geometry = InkGeometry{{{PointF{20.0, 200.0}, PointF{60.0, 240.0},
                                                    PointF{100.0, 210.0}}}};
                break;
            case 3: annotation.geometry = TextNoteGeometry{TextNoteIcon::Note, false}; break;
            default:
                annotation.geometry = ShapeGeometry{ShapeKind::Circle};
                annotation.interiorColor = ColorRgb{1.0, 1.0, 0.6};
                break;
        }

        const WriteResult result = document.addAnnotation(0, annotation);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const auto subtype = document.subtypeName(0, 0);
        QVERIFY(subtype.has_value());
        QCOMPARE(QString::fromStdString(*subtype), expectedSubtype);

        const auto ap = document.appearanceStream(0, 0);
        QVERIFY(ap.has_value());
        QVERIFY(!ap->empty());
        const ContentStreamReport report = checkContentStream(QByteArray::fromStdString(*ap));
        QVERIFY2(report.valid, qPrintable(report.error));
    }

    // 多則註解依序寫入，索引必須遞增且彼此的 /AP 互不覆蓋。
    void multipleAnnotationsKeepIndependentAppearances() {
        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));

        Annotation first = makeTwoLineHighlight();
        Annotation second = makeTwoLineHighlight();
        second.id = "alioth-0002";
        second.geometry = TextMarkupGeometry{TextMarkupKind::Underline,
                                             {quadFromPageRect(RectF{20.0, 100.0, 90.0, 118.0})}};

        const WriteResult a = document.addAnnotation(0, first);
        const WriteResult b = document.addAnnotation(0, second);
        QVERIFY2(a.ok, a.diagnostic.c_str());
        QVERIFY2(b.ok, b.diagnostic.c_str());
        QCOMPARE(a.index, 0);
        QCOMPARE(b.index, 1);
        QCOMPARE(document.annotationCount(0), 2);

        const auto apA = document.appearanceStream(0, 0);
        const auto apB = document.appearanceStream(0, 1);
        QVERIFY(apA.has_value() && apB.has_value());
        QVERIFY(*apA != *apB);
    }

    // 產生器失敗時不得建立半成品註解。
    void rejectsAnnotationWithoutValidGeometry() {
        const QByteArray pdf = makeSinglePagePdf();
        AnnotationDocument document;
        QVERIFY(document.openFromMemory(pdf.constData(), static_cast<std::size_t>(pdf.size())));

        Annotation annotation{};
        annotation.geometry = TextMarkupGeometry{TextMarkupKind::Highlight, {}};
        const WriteResult result = document.addAnnotation(0, annotation);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QCOMPARE(document.annotationCount(0), 0);
    }
};

QTEST_APPLESS_MAIN(TestAnnotationWriter)
#include "test_annotation_writer.moc"
