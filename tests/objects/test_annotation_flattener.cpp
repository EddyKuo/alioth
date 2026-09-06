// 註解攤平測試(PRD-ANN-013)。
//
// 攤平後必須能用「重新開啟並讀內容串流」驗證幾何真的變成了頁面內容,
// 而不是還躺在 /Annots 裡等著被編輯——這是本檔案存在的理由,也呼應
// CLAUDE.md 對這個工作包的要求:「攤平後以文字擷取或渲染驗證註解真的
// 變成頁面內容」。

#include <QtTest>

#include <string>

#include "domain/annotation.h"
#include "domain/redaction.h"
#include "engine/annotations/annotation_document.h"
#include "engine/objects/annotation_flattener.h"
#include "engine/objects/incremental_appender.h"
#include "object_fixture.h"

using namespace alioth::engine::objects;
using alioth::domain::Annotation;
using alioth::domain::ColorRgb;
using alioth::domain::IrreversibleConsent;
using alioth::domain::RectF;
using alioth::domain::ShapeGeometry;
using alioth::domain::ShapeKind;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;

namespace {

Annotation makeFilledSquare() {
    Annotation annotation{};
    annotation.rect = RectF{20, 20, 80, 80};
    annotation.color = ColorRgb{1.0, 0.0, 0.0};
    annotation.interiorColor = ColorRgb{0.0, 1.0, 0.0};
    annotation.border.width = 2.0;
    annotation.geometry = ShapeGeometry{ShapeKind::Square};
    return annotation;
}

Annotation makeSemiTransparentHighlight() {
    Annotation annotation{};
    annotation.color = ColorRgb{1.0, 1.0, 0.0};
    annotation.opacity = 0.4;
    annotation.geometry = alioth::domain::TextMarkupGeometry{
        alioth::domain::TextMarkupKind::Highlight,
        {alioth::domain::quadFromPageRect(RectF{10, 100, 150, 120})}};
    return annotation;
}

PdfObject entryOf(const PdfObject& object, const char* key) {
    const PdfDictionary* dict = object.asDictionary();
    if (dict == nullptr) return PdfObject{};
    const PdfObject* value = dict->find(key);
    return value == nullptr ? PdfObject{} : *value;
}

std::size_t arraySize(const PdfObject& object) {
    const PdfArray* array = object.asArray();
    return array == nullptr ? 0 : array->size();
}

}  // namespace

class TestAnnotationFlattener : public QObject {
    Q_OBJECT

private slots:
    void flattenedShapeBecomesPageContent() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const FlattenResult result =
            flattenAnnotation(appender, 0, makeFilledSquare(), IrreversibleConsent::confirmed());
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        // 純附加前提:原檔位元組必須原封不動地是輸出的前綴。
        QCOMPARE(built.bytes.compare(0, source.size(), source), 0);

        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfRef page = reopened.source().pages().front();
        const PdfObject pageObject = reopened.source().object(page.number);

        // 沒有走 /Annots——攤平後不再是可編輯的互動註解。
        const PdfObject annots = reopened.source().resolve(entryOf(pageObject, "Annots"));
        QCOMPARE(arraySize(annots), std::size_t{0});

        // 新內容串流必須真的含有畫矩形與填色的運算子。
        const PdfObject contents = reopened.source().resolve(entryOf(pageObject, "Contents"));
        QCOMPARE(arraySize(contents), std::size_t{2});
        const PdfObject newStream =
            reopened.source().object(contents.asArray()->at(1).asRef().number);
        QVERIFY(newStream.isStream());
        const std::string& data = newStream.asStream()->data;
        QVERIFY(data.find(" re\n") != std::string::npos);
        QVERIFY(data.find("0 1 0 rg") != std::string::npos);  // 綠色填色

        // PDFium 仍然能開啟這份檔案,而且看不到任何互動註解。
        alioth::engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(built.bytes.data(), built.bytes.size()));
        QCOMPARE(document.annotationCount(0), 0);
    }

    void flattenedHighlightRegistersPageLevelExtGState() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const FlattenResult result = flattenAnnotation(
            appender, 0, makeSemiTransparentHighlight(), IrreversibleConsent::confirmed());
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfRef page = reopened.source().pages().front();
        const PdfObject pageObject = reopened.source().object(page.number);
        const PdfObject resources = reopened.source().resolve(entryOf(pageObject, "Resources"));
        const PdfObject gs = entryOf(entryOf(resources, "ExtGState"), "GS0");
        QVERIFY(gs.isDictionary());

        const PdfObject contents = reopened.source().resolve(entryOf(pageObject, "Contents"));
        const PdfObject newStream =
            reopened.source().object(contents.asArray()->at(1).asRef().number);
        QVERIFY(newStream.asStream()->data.find("/GS0 gs") != std::string::npos);
    }

    void invalidGeometryFailsExplicitly() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        Annotation empty;
        empty.geometry = alioth::domain::TextMarkupGeometry{};  // 沒有 QuadPoints
        const FlattenResult result =
            flattenAnnotation(appender, 0, empty, IrreversibleConsent::confirmed());
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void pageOutOfRangeFailsExplicitly() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const FlattenResult result = flattenAnnotation(appender, 9, makeFilledSquare(),
                                                        IrreversibleConsent::confirmed());
        QVERIFY(!result.ok);
    }
};

QTEST_APPLESS_MAIN(TestAnnotationFlattener)
#include "test_annotation_flattener.moc"
