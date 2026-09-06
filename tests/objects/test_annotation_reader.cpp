// 從 PDF 讀回註解（PRD-ANN-013 匯出路徑的前半）。
//
// 這支測試釘住的是寫入與讀取的對稱性：寫得出來的欄位就要讀得回來。
// 不對稱在正常使用時完全看不見——註解在畫面上是對的，只有匯出給別人之後
// 對方才會發現形狀不見了，而那時已經來不及。

#include <QtTest>

#include <string>

#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/annotation_reader.h"
#include "engine/objects/incremental_appender.h"
#include "object_fixture.h"

using namespace alioth::engine::objects;
using alioth::domain::Annotation;
using alioth::domain::AnnotationType;
using alioth::domain::ColorRgb;
using alioth::domain::InkGeometry;
using alioth::domain::PointF;
using alioth::domain::quadFromPageRect;
using alioth::domain::RectF;
using alioth::domain::ShapeGeometry;
using alioth::domain::ShapeKind;
using alioth::domain::TextMarkupGeometry;
using alioth::domain::TextMarkupKind;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;

class TestAnnotationReader : public QObject {
    Q_OBJECT

private slots:
    void readsBackEverythingItWrote();
    void skipsUnsupportedSubtypesButKeepsIndexOnPageAligned();
};

void TestAnnotationReader::readsBackEverythingItWrote() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);

    Annotation highlight;
    highlight.id = "alioth-1";
    highlight.author = "審閱者";
    highlight.contents = "這段要再確認";
    highlight.subject = "疑問";
    highlight.color = ColorRgb{1.0, 0.85, 0.0};
    highlight.opacity = 0.4;
    highlight.creationDate = alioth::domain::PdfDate{2026, 9, 6, 10, 15, 0, 8, 0};
    highlight.modifiedDate = highlight.creationDate;
    highlight.geometry = TextMarkupGeometry{TextMarkupKind::Highlight,
                                            {quadFromPageRect(RectF{20, 100, 120, 116}),
                                             quadFromPageRect(RectF{20, 80, 90, 96})}};
    QVERIFY(writeAnnotation(appender, 0, highlight).ok);

    Annotation ink;
    ink.rect = RectF{0, 0, 200, 200};
    ink.border.width = 3.0;
    ink.geometry = InkGeometry{{{PointF{10, 10}, PointF{20, 30}, PointF{40, 25}}}};
    QVERIFY(writeAnnotation(appender, 0, ink).ok);

    const BuildResult built = appender.build();
    QVERIFY(built.ok);
    const std::string bytes(reinterpret_cast<const char*>(built.bytes.data()), built.bytes.size());

    IncrementalAppender reader;
    QCOMPARE(reader.open(bytes), SourceStatus::Ok);
    const std::vector<PageAnnotation> read = readAllAnnotations(reader);
    QCOMPARE(read.size(), std::size_t(2));

    const Annotation& back = read[0].annotation;
    QCOMPARE(read[0].pageIndex, 0);
    QCOMPARE(back.type(), AnnotationType::Highlight);
    QCOMPARE(QString::fromStdString(back.author), QStringLiteral("審閱者"));
    QCOMPARE(QString::fromStdString(back.contents), QStringLiteral("這段要再確認"));
    QCOMPARE(QString::fromStdString(back.subject), QStringLiteral("疑問"));
    QCOMPARE(back.id, std::string("alioth-1"));
    QVERIFY(qFuzzyCompare(back.opacity, 0.4));
    QCOMPARE(back.creationDate.year, 2026);
    QCOMPARE(back.creationDate.minute, 15);
    const auto* markup = std::get_if<TextMarkupGeometry>(&back.geometry);
    QVERIFY(markup != nullptr);
    QCOMPARE(markup->quads.size(), std::size_t(2));

    const auto* strokes = std::get_if<InkGeometry>(&read[1].annotation.geometry);
    QVERIFY(strokes != nullptr);
    QCOMPARE(strokes->strokes.size(), std::size_t(1));
    QCOMPARE(strokes->strokes[0].size(), std::size_t(3));
    QVERIFY(qFuzzyCompare(strokes->strokes[0][2].x, 40.0));
    QVERIFY(qFuzzyCompare(read[1].annotation.border.width, 3.0));
}

void TestAnnotationReader::skipsUnsupportedSubtypesButKeepsIndexOnPageAligned() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);

    // 便利貼會連帶寫出一則 /Popup，而 /Popup 也是 /Annots 的成員。
    // 讀取端必須跳過它，但**不能因此重新編號**——indexOnPage 是刪除與
    // 編輯註釋定位用的座標，與註解清單共用，錯開一格就會動到隔壁那則。
    Annotation note;
    note.rect = RectF{40, 700, 60, 720};
    note.geometry = alioth::domain::TextNoteGeometry{};
    const AnnotationWriteResult written = writeAnnotation(appender, 0, note);
    QVERIFY(written.ok);
    QVERIFY(written.popupObject != 0);

    Annotation square;
    square.rect = RectF{10, 10, 110, 60};
    square.geometry = ShapeGeometry{ShapeKind::Square};
    QVERIFY(writeAnnotation(appender, 0, square).ok);

    const BuildResult built = appender.build();
    QVERIFY(built.ok);
    const std::string bytes(reinterpret_cast<const char*>(built.bytes.data()), built.bytes.size());

    IncrementalAppender reader;
    QCOMPARE(reader.open(bytes), SourceStatus::Ok);
    const std::vector<PageAnnotation> read = readAllAnnotations(reader);

    QCOMPARE(read.size(), std::size_t(2));
    QCOMPARE(read[0].annotation.type(), AnnotationType::Text);
    QCOMPARE(read[0].indexOnPage, 0);
    QCOMPARE(read[1].annotation.type(), AnnotationType::Square);
    // /Popup 佔掉了 /Annots 的第 1 格，所以方框是第 2 格而不是第 1 格。
    QCOMPARE(read[1].indexOnPage, 2);
}

QTEST_APPLESS_MAIN(TestAnnotationReader)
#include "test_annotation_reader.moc"
