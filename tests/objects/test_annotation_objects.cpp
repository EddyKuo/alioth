// 註解與內容串流的物件層寫入測試（ADR-002 驗收條件 3 與 4）。
//
// 這裡的重點是 PDFium 的 API 做不到的那幾件事：/AP 的 /Resources（螢光筆的
// Multiply 混合）、/IRT 回覆串、/L 與 /BS /D，以及把內容串流掛上 /Contents。
// 驗證分兩路：用我們的剖析器檢查物件結構是否符合 ISO 32000，
// 再用 PDFium 重新開啟確認檔案本身仍然可讀。

#include <QtTest>

#include <string>
#include <variant>

#include "engine/annotations/annotation_document.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/incremental_appender.h"
#include "object_fixture.h"

using namespace alioth::engine::objects;
using alioth::domain::Annotation;
using alioth::domain::BorderStyleKind;
using alioth::domain::ColorRgb;
using alioth::domain::LineGeometry;
using alioth::domain::PointF;
using alioth::domain::quadFromPageRect;
using alioth::domain::RectF;
using alioth::domain::TextMarkupGeometry;
using alioth::domain::TextMarkupKind;
using alioth::test::makeFixturePdf;
using alioth::test::PdfFixtureOptions;
using alioth::test::toStdString;

namespace {

Annotation makeHighlight() {
    Annotation annotation{};
    annotation.id = "alioth-1";
    annotation.author = "審閱者";
    annotation.contents = "這段要再確認";
    annotation.color = ColorRgb{1.0, 1.0, 0.0};
    annotation.opacity = 0.4;
    annotation.creationDate = alioth::domain::PdfDate{2026, 9, 5, 14, 30, 0, 8, 0};
    annotation.modifiedDate = annotation.creationDate;
    annotation.geometry =
        TextMarkupGeometry{TextMarkupKind::Highlight, {quadFromPageRect(RectF{20, 100, 120, 116})}};
    return annotation;
}

Annotation makeDashedLine() {
    Annotation annotation{};
    annotation.color = ColorRgb{0.0, 0.0, 1.0};
    annotation.border.width = 2.0;
    annotation.border.style = BorderStyleKind::Dashed;
    annotation.border.dashPattern = {4.0, 2.0};
    annotation.geometry = LineGeometry{PointF{20, 20}, PointF{160, 90},
                                       alioth::domain::LineEnding::None,
                                       alioth::domain::LineEnding::OpenArrow};
    return annotation;
}

// 重新開啟輸出並取出指定物件，等於從第三方視角讀我們寫的位元組。
PdfObject reopenObject(const std::string& bytes, int number) {
    IncrementalAppender appender;
    if (appender.open(bytes) != SourceStatus::Ok) return PdfObject{};
    return appender.source().object(number);
}

// 取值一律回傳複本而不是指標。PdfSourceDocument::object() 回傳的是暫存物件，
// 對它取出的內部指標在該敘述結束後就懸空了——那種錯誤讀起來完全正常，
// 只會讓斷言比對到一堆零。
PdfObject entryOf(const PdfObject& object, const char* key) {
    const PdfDictionary* dict = object.asDictionary();
    if (dict == nullptr) return PdfObject{};
    const PdfObject* value = dict->find(key);
    return value == nullptr ? PdfObject{} : *value;
}

QString nameOf(const PdfObject& object, const char* key) {
    return QString::fromStdString(entryOf(object, key).asName());
}

bool hasKey(const PdfObject& object, const char* key) {
    const PdfDictionary* dict = object.asDictionary();
    return dict != nullptr && dict->has(key);
}

std::size_t arraySize(const PdfObject& object) {
    const PdfArray* array = object.asArray();
    return array == nullptr ? 0 : array->size();
}

PdfObject elementOf(const PdfObject& object, std::size_t index) {
    const PdfArray* array = object.asArray();
    if (array == nullptr || index >= array->size()) return PdfObject{};
    return array->at(index);
}

int pdfiumAnnotationCount(const std::string& bytes, QString* firstSubtype = nullptr) {
    alioth::engine::annotations::AnnotationDocument document;
    if (!document.openFromMemory(bytes.data(), bytes.size())) return -1;
    const int count = document.annotationCount(0);
    if (firstSubtype != nullptr && count > 0) {
        if (const auto subtype = document.subtypeName(0, 0)) {
            *firstSubtype = QString::fromStdString(*subtype);
        }
    }
    return count;
}

}  // namespace

class TestAnnotationObjects : public QObject {
    Q_OBJECT

private slots:
    void highlightAppearanceCarriesItsOwnResources() {
        // ADR-002 的核心：FPDFAnnot_SetAP 建不出 /Resources，串流內的 /GS0
        // 因此是懸空名稱，螢光筆的 Multiply 混合直接消失。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeHighlight());
        QVERIFY2(written.ok, written.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const PdfObject appearance = reopenObject(built.bytes, written.appearanceObject);
        QVERIFY(appearance.isStream());
        QCOMPARE(nameOf(appearance, "Subtype"), QStringLiteral("Form"));

        const PdfObject state = entryOf(entryOf(entryOf(appearance, "Resources"), "ExtGState"), "GS0");
        QVERIFY(state.isDictionary());
        QCOMPARE(nameOf(state, "BM"), QStringLiteral("Multiply"));
        QCOMPARE(entryOf(state, "CA").asNumber(), 0.4);
        QCOMPARE(entryOf(state, "ca").asNumber(), 0.4);

        // 內容串流必須真的引用那個資源，否則寫了 /Resources 也是白寫。
        QVERIFY(appearance.asStream()->data.find("/GS0 gs") != std::string::npos);
    }

    void annotationDictionaryHasTheRequiredKeys() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeHighlight());
        QVERIFY(written.ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Type"), QStringLiteral("Annot"));
        QCOMPARE(nameOf(annot, "Subtype"), QStringLiteral("Highlight"));
        // CLAUDE.md 硬性限制 4 列出的必填鍵。
        for (const char* key : {"Rect", "C", "CA", "F", "BS", "QuadPoints", "AP", "NM", "T",
                                "Contents", "CreationDate", "M", "P", "Popup"}) {
            QVERIFY2(hasKey(annot, key), key);
        }
        QCOMPARE(arraySize(entryOf(annot, "QuadPoints")), std::size_t{8});
        QCOMPARE(entryOf(entryOf(annot, "AP"), "N").asRef().number, written.appearanceObject);

        // 非 ASCII 的作者與內容必須是 UTF-16BE，否則 Acrobat 顯示成亂碼。
        QVERIFY(serialize(entryOf(annot, "T")).rfind("<FEFF", 0) == 0);
        QVERIFY(serialize(entryOf(annot, "Contents")).rfind("<FEFF", 0) == 0);
    }

    void popupPointsBackToItsParent() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeHighlight());
        QVERIFY(written.ok);
        QVERIFY(written.popupObject != 0);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject popup = reopenObject(built.bytes, written.popupObject);
        QCOMPARE(nameOf(popup, "Subtype"), QStringLiteral("Popup"));
        QCOMPARE(entryOf(popup, "Parent").asRef().number, written.annotationObject);

        // 註解與彈出視窗都必須掛在頁面的 /Annots 上。
        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfRef page = reopened.source().pages().front();
        const PdfObject pageObject = reopened.source().object(page.number);
        const PdfObject annots = reopened.source().resolve(entryOf(pageObject, "Annots"));
        QCOMPARE(arraySize(annots), std::size_t{2});
    }

    void replyAnnotationReferencesItsParentThroughIrt() {
        // PRD-ANN-007：回覆串在 Acrobat 必須顯示為串接而非獨立註解。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const AnnotationWriteResult parent = writeAnnotation(appender, 0, makeHighlight());
        QVERIFY(parent.ok);

        Annotation replyModel = makeHighlight();
        replyModel.id = "alioth-2";
        replyModel.contents = "已確認";
        AnnotationWriteOptions options;
        options.inReplyToObject = parent.annotationObject;
        const AnnotationWriteResult reply = writeAnnotation(appender, 0, replyModel, options);
        QVERIFY2(reply.ok, reply.diagnostic.c_str());
        // 回覆共用父註解的視窗，自己不帶 /Popup。
        QCOMPARE(reply.popupObject, 0);

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, reply.annotationObject);
        QCOMPARE(entryOf(annot, "IRT").asRef().number, parent.annotationObject);
        QCOMPARE(nameOf(annot, "RT"), QStringLiteral("R"));
        QVERIFY(!hasKey(annot, "Popup"));

        // 同一頁寫第二則註解時不得把第一則從 /Annots 上洗掉。
        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfRef page = reopened.source().pages().front();
        const PdfObject pageObject = reopened.source().object(page.number);
        const PdfObject annots = reopened.source().resolve(entryOf(pageObject, "Annots"));
        QCOMPARE(arraySize(annots), std::size_t{3});
    }

    void replyAnnotationCanCarryReviewState() {
        // PRD-ANN-007「已接受／已拒絕／已完成」:狀態必須寫在回覆註解上
        // (/StateModel /State),不是改寫父註解本身,否則「誰在何時標記」
        // 這件事就無從稽核。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const AnnotationWriteResult parent = writeAnnotation(appender, 0, makeHighlight());
        QVERIFY(parent.ok);

        Annotation stateReply = makeHighlight();
        stateReply.id = "alioth-state-1";
        stateReply.contents = {};
        AnnotationWriteOptions options;
        options.inReplyToObject = parent.annotationObject;
        options.stateModel = "Review";
        options.state = "Accepted";
        const AnnotationWriteResult reply = writeAnnotation(appender, 0, stateReply, options);
        QVERIFY2(reply.ok, reply.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, reply.annotationObject);
        const auto textOf = [](const PdfObject& value) -> std::string {
            const PdfString* str = std::get_if<PdfString>(&value.value());
            return str == nullptr ? std::string{} : str->bytes;
        };
        QCOMPARE(QString::fromStdString(textOf(entryOf(annot, "StateModel"))),
                 QStringLiteral("Review"));
        QCOMPARE(QString::fromStdString(textOf(entryOf(annot, "State"))),
                 QStringLiteral("Accepted"));

        // 父註解本身完全沒有被改動——狀態只掛在回覆上。
        const PdfObject parentAnnot = reopenObject(built.bytes, parent.annotationObject);
        QVERIFY(!hasKey(parentAnnot, "StateModel"));
        QVERIFY(!hasKey(parentAnnot, "State"));
    }

    void stateWithoutReplyIsIgnored() {
        // stateModel/state 只有在 inReplyToObject 有值時才有意義;
        // 單獨設定時必須被忽略,而不是寫出一個沒有 IRT 的懸空狀態。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        AnnotationWriteOptions options;
        options.stateModel = "Review";
        options.state = "Accepted";
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeHighlight(), options);
        QVERIFY(written.ok);

        const BuildResult built = appender.build();
        QVERIFY(built.ok);
        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QVERIFY(!hasKey(annot, "StateModel"));
        QVERIFY(!hasKey(annot, "State"));
    }

    void lineAnnotationsCarryLAndDashPattern() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeDashedLine());
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Subtype"), QStringLiteral("Line"));

        const PdfObject line = entryOf(annot, "L");
        QCOMPARE(arraySize(line), std::size_t{4});
        QCOMPARE(elementOf(line, 0).asNumber(), 20.0);
        QCOMPARE(elementOf(line, 3).asNumber(), 90.0);
        QCOMPARE(QString::fromStdString(elementOf(entryOf(annot, "LE"), 1).asName()),
                 QStringLiteral("OpenArrow"));

        const PdfObject bs = entryOf(annot, "BS");
        QCOMPARE(nameOf(bs, "S"), QStringLiteral("D"));
        QCOMPARE(arraySize(entryOf(bs, "D")), std::size_t{2});
        QCOMPARE(entryOf(bs, "W").asNumber(), 2.0);
    }

    void pdfiumReadsTheAppendedAnnotations() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        QVERIFY(writeAnnotation(appender, 0, makeHighlight()).ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        QString subtype;
        // 註解本體加上 /Popup，PDFium 會列出兩則。
        QCOMPARE(pdfiumAnnotationCount(built.bytes, &subtype), 2);
        QCOMPARE(subtype, QStringLiteral("Highlight"));
    }

    void annotationsCanBeWrittenToLaterPages() {
        PdfFixtureOptions options;
        options.pageCount = 3;
        const std::string source = toStdString(makeFixturePdf(options));
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 2, makeHighlight());
        QVERIFY(written.ok);

        const PdfObject annot = appender.currentObject(written.annotationObject);
        QCOMPARE(entryOf(annot, "P").asRef().number, appender.source().pages()[2].number);

        // 頁碼越界必須明確失敗，而不是寫到別頁去。
        const AnnotationWriteResult bad = writeAnnotation(appender, 7, makeHighlight());
        QVERIFY(!bad.ok);
        QVERIFY(!bad.diagnostic.empty());
    }

    void contentStreamIsAppendedToContentsArray() {
        // PRD-PAGE-004 的「永久套用」：原本的 /Contents 是單一串流參照，
        // 必須轉成陣列並保留原本那一份，否則頁面原有內容整個消失。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        ContentAppendOptions options;
        options.fonts.push_back(ContentFontRequest{"F0", "Helvetica"});
        const ContentAppendResult result =
            appendPageContent(appender, 0, "BT /F0 10 Tf 20 20 Td (BATES-000123) Tj ET\n", options);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfRef page = reopened.source().pages().front();
        const PdfObject pageObject = reopened.source().object(page.number);

        const PdfObject contents = entryOf(pageObject, "Contents");
        QCOMPARE(arraySize(contents), std::size_t{2});
        QCOMPARE(elementOf(contents, 0).asRef().number, 4);  // 原本的內容串流仍在第一位
        QCOMPARE(elementOf(contents, 1).asRef().number, result.contentObject);

        // 新串流必須以 q/Q 包住，否則圖形狀態會外溢到後續內容。
        const PdfObject stream = reopened.source().object(result.contentObject);
        QVERIFY(stream.isStream());
        QVERIFY(stream.asStream()->data.find("q\n") != std::string::npos);
        QVERIFY(stream.asStream()->data.find("Q\n") != std::string::npos);

        const PdfObject fontRef = entryOf(entryOf(entryOf(pageObject, "Resources"), "Font"), "F0");
        const PdfObject font = reopened.source().object(fontRef.asRef().number);
        QCOMPARE(nameOf(font, "BaseFont"), QStringLiteral("Helvetica"));
        QCOMPARE(nameOf(font, "Encoding"), QStringLiteral("WinAnsiEncoding"));

        alioth::engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(built.bytes.data(), built.bytes.size()));
        QCOMPARE(document.pageCount(), 1);
    }

    void inheritedResourcesAreCopiedNotShadowed() {
        // 頁面沒有 /Resources 時直接新建一個，會遮蔽從 /Pages 繼承來的資源，
        // 頁面原本用到的字型與影像會全部消失。
        PdfFixtureOptions options;
        options.inheritResources = true;
        const std::string source = toStdString(makeFixturePdf(options));
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        ContentAppendOptions contentOptions;
        contentOptions.fonts.push_back(ContentFontRequest{"F0", "Times-Roman"});
        QVERIFY(appendPageContent(appender, 0, "BT ET\n", contentOptions).ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        IncrementalAppender reopened;
        QCOMPARE(reopened.open(built.bytes), SourceStatus::Ok);
        const PdfRef page = reopened.source().pages().front();
        const PdfObject pageObject = reopened.source().object(page.number);
        const PdfObject resources = entryOf(pageObject, "Resources");
        QVERIFY(hasKey(resources, "ProcSet"));  // 繼承來的項目仍在
        QVERIFY(hasKey(entryOf(resources, "Font"), "F0"));
    }

    void nonAsciiContentIsRefusedUntilFontEmbeddingIsDecided() {
        // CJK 字型內嵌的授權策略尚未定案（CLAUDE.md 待決策清單），
        // 因此明確失敗，不輸出會變成亂碼或缺字的位元組。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        ContentAppendOptions options;
        options.fonts.push_back(ContentFontRequest{"F0", "Helvetica"});
        const ContentAppendResult result =
            appendPageContent(appender, 0, "BT /F0 10 Tf (編號 000123) Tj ET\n", options);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void onlyStandard14FontsAreAccepted() {
        QVERIFY(isStandard14Font("Helvetica-BoldOblique"));
        QVERIFY(isStandard14Font("ZapfDingbats"));
        QVERIFY(!isStandard14Font("MSJhengHei"));

        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        ContentAppendOptions options;
        options.fonts.push_back(ContentFontRequest{"F0", "MSJhengHei"});
        const ContentAppendResult result = appendPageContent(appender, 0, "BT ET\n", options);
        QVERIFY(!result.ok);
    }

    void encryptedDocumentsRejectBothChannels() {
        PdfFixtureOptions options;
        options.encrypted = true;
        IncrementalAppender appender;
        QCOMPARE(appender.open(toStdString(makeFixturePdf(options))), SourceStatus::Encrypted);
        QVERIFY(!writeAnnotation(appender, 0, makeHighlight()).ok);
        QVERIFY(!appendPageContent(appender, 0, "BT ET\n").ok);
    }
};

QTEST_APPLESS_MAIN(TestAnnotationObjects)
#include "test_annotation_objects.moc"
