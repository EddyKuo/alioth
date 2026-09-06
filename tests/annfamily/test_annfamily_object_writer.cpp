// engine::objects::writeAnnotation 對 FreeText 家族與 Caret 的物件層測試
// （WP24：PRD-ANN-005/017/018/019/021/022，走 ADR-002 物件層通道）。
//
// 驗證重點：/Resources /Font /Helv 真的被寫進 /AP（不是只有內容串流引用了
// 一個懸空名稱）、/DA /IT /CL /LE /Sy 這些 FPDFAnnot_SetAP 寫不出來的鍵都在，
// 以及輸出通過 qpdf --check（PRD §9 明列的驗收條件）。

#include <QtTest>

#include <QCoreApplication>
#include <QDir>

#include "engine/fonts/cjk_font_library.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/incremental_appender.h"
#include "objects/object_fixture.h"
#include "qa/qpdf_check.h"

using namespace alioth::engine::objects;
using namespace alioth::domain;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;
using alioth::test::toByteArray;
using alioth::test::describeQpdfFailure;
using alioth::test::qpdfSkipReason;
using alioth::test::runQpdfCheckOnBytes;
using alioth::test::QpdfStatus;

namespace {

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

PdfObject reopenObject(const std::string& bytes, int number) {
    IncrementalAppender appender;
    if (appender.open(bytes) != SourceStatus::Ok) return PdfObject{};
    return appender.source().object(number);
}

Annotation makeTextBox() {
    Annotation annotation;
    annotation.id = "wp24-textbox";
    annotation.rect = RectF{50, 500, 250, 580};
    annotation.color = ColorRgb{0.2, 0.2, 0.2};
    annotation.border.width = 1.0;

    FreeTextGeometry geometry;
    geometry.text = "Reviewed";
    geometry.fontSize = 14.0;
    geometry.textColor = ColorRgb{0.0, 0.0, 0.0};
    geometry.align = TextAlign::Center;
    geometry.intent = FreeTextIntent::TextBox;
    annotation.geometry = geometry;
    return annotation;
}

Annotation makeTypewriter() {
    Annotation annotation = makeTextBox();
    annotation.id = "wp24-typewriter";
    annotation.rect = RectF{50, 400, 250, 440};
    auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
    geometry.intent = FreeTextIntent::Typewriter;
    return annotation;
}

Annotation makeCallout() {
    Annotation annotation = makeTextBox();
    annotation.id = "wp24-callout";
    annotation.rect = RectF{50, 200, 170, 260};

    auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
    geometry.intent = FreeTextIntent::Callout;
    CalloutLine line;
    line.start = PointF{50, 230};
    line.knee = PointF{20, 260};
    line.end = PointF{5, 320};
    line.ending = LineEnding::OpenArrow;
    geometry.callout = line;
    return annotation;
}

Annotation makeCaret(CaretSymbol symbol) {
    Annotation annotation;
    annotation.id = symbol == CaretSymbol::Paragraph ? "wp24-caret-p" : "wp24-caret-none";
    annotation.rect = RectF{300, 500, 320, 520};
    annotation.color = ColorRgb{0.8, 0.0, 0.0};
    annotation.geometry = CaretGeometry{symbol};
    return annotation;
}

}  // namespace

class TestAnnfamilyObjectWriter : public QObject {
    Q_OBJECT

private slots:
    void textBoxAppearanceCarriesItsOwnFontResource() {
        // FPDFAnnot_SetAP 建不出 /Resources；沒有這個字典，/AP 內容串流裡的
        // /Helv 就是懸空名稱，文字完全不會被畫出來（CLAUDE.md 硬性限制 1）。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeTextBox());
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const PdfObject appearance = reopenObject(built.bytes, written.appearanceObject);
        QVERIFY(appearance.isStream());
        const PdfObject font = entryOf(entryOf(entryOf(appearance, "Resources"), "Font"), "Helv");
        QVERIFY(font.isDictionary());
        QCOMPARE(nameOf(font, "BaseFont"), QStringLiteral("Helvetica"));
        QCOMPARE(nameOf(font, "Encoding"), QStringLiteral("WinAnsiEncoding"));

        // 內容串流必須真的引用 /Helv，否則寫了 /Resources 也是白寫。
        QVERIFY(appearance.asStream()->data.find("/Helv") != std::string::npos);
        QVERIFY(appearance.asStream()->data.find(" Tf") != std::string::npos);
    }

    // 中文文字方塊的完整鏈路（ADR-007）：子集 → 內嵌 → /Resources /Font /CJK
    // → 內容串流引用。
    //
    // 這一條要攔的是**中文整段消失**：內容串流引用了 /CJK，但 /Resources 裡
    // 沒有註冊那個名稱。那不是亂碼、不是錯誤訊息，就是什麼都不畫——
    // 而 generateAppearance 與 qpdf --check 都會說一切正常。
    void cjkTextBoxEmbedsAndRegistersTheFont() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("這台機器上沒有 CJK 字型，跳過");
        }

        Annotation annotation = makeTextBox();
        auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
        geometry.text = "\xE5\xAF\xA9\xE9\x96\xB1\xE5\xAE\x8C\xE7\x95\xA2";  // 審閱完畢

        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const AnnotationWriteResult written = writeAnnotation(appender, 0, annotation);
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const PdfObject appearance = reopenObject(built.bytes, written.appearanceObject);
        QVERIFY(appearance.isStream());

        // 內容串流引用 /CJK。
        QVERIFY2(appearance.asStream()->data.find("/CJK") != std::string::npos,
                 "內容串流沒有切換到 CJK 字型");

        // /Resources /Font /CJK 真的存在，而且是 Type0 + Identity-H。
        const PdfObject font = entryOf(entryOf(entryOf(appearance, "Resources"), "Font"), "CJK");
        QVERIFY2(font.isDictionary() || font.isRef(),
                 "內容串流引用了 /CJK，但 /Resources 裡沒有註冊它——中文會整段消失");

        const PdfObject type0 =
            font.isRef() ? reopenObject(built.bytes, font.asRef().number) : font;
        QCOMPARE(nameOf(type0, "Subtype"), QStringLiteral("Type0"));
        QCOMPARE(nameOf(type0, "Encoding"), QStringLiteral("Identity-H"));
        // 沒有 /ToUnicode 的話，中文看得到但複製不出來也搜尋不到。
        QVERIFY(entryOf(type0, "ToUnicode").isRef());

        // 增量寫入：原檔位元組不變，既有簽章才不會從「有效、簽章後有變更」掉成無效。
        QCOMPARE(built.bytes.compare(0, source.size(), source), 0);

        // 交給 qpdf 獨立判定。我們自己的序列化器認為對的，不代表別人讀得進去。
        const QString path = QDir::temp().filePath(
            QStringLiteral("alioth-cjk-%1.pdf").arg(QCoreApplication::applicationPid()));
        const alioth::test::QpdfCheckResult check =
            runQpdfCheckOnBytes(path, toByteArray(built.bytes));
        if (check.status == QpdfStatus::NotAvailable) QSKIP(qpdfSkipReason().constData());
        QVERIFY2(check.clean(),
                 describeQpdfFailure(QStringLiteral("CJK 文字方塊"), check).constData());
    }

    // Callout 的中文（PRD-ANN-022）。
    //
    // Callout 與 Text Box 共用同一條繪製流程，所以理論上 CJK「應該也會通」——
    // 但 needsCjkFont 是在流程末端由一個旗標決定的，而 Callout 多了一段引線
    // 繪製。共用不等於驗過：真正會發生的失敗是引線畫出來了、文字整段沒有，
    // 而那在 qpdf --check 與 generateAppearance 眼中都是正常的。
    void cjkCalloutEmbedsTheFontToo() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("這台機器上沒有 CJK 字型，跳過");
        }

        Annotation annotation = makeTextBox();
        annotation.id = "wp24-callout-cjk";
        auto& geometry = std::get<FreeTextGeometry>(annotation.geometry);
        geometry.text = "請確認";  // 請確認
        geometry.intent = FreeTextIntent::Callout;
        CalloutLine line;
        line.start = PointF{50, 540};
        line.end = PointF{20, 620};
        geometry.callout = line;

        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        const AnnotationWriteResult written = writeAnnotation(appender, 0, annotation);
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const PdfObject appearance = reopenObject(built.bytes, written.appearanceObject);
        QVERIFY(appearance.isStream());
        QVERIFY2(appearance.asStream()->data.find("/CJK") != std::string::npos,
                 "Callout 的內容串流沒有切換到 CJK 字型");
        const PdfObject font = entryOf(entryOf(entryOf(appearance, "Resources"), "Font"), "CJK");
        QVERIFY2(font.isDictionary() || font.isRef(),
                 "Callout 引用了 /CJK 但 /Resources 沒有註冊它——中文會整段消失");

        // 引線必須還在：只驗字型會讓「文字對了但引線不見了」通過。
        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QVERIFY2(entryOf(annot, "CL").isArray(), "Callout 少了 /CL 引線");
    }

    void asciiTextBoxDoesNotEmbedTheCjkFont() {
        // 純英文的註解不該把字型子集拖進檔案。內嵌一份 170 KB 的子集只為了
        // 寫 "Reviewed"，會讓每一則英文註解都把檔案撐大。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeTextBox());
        QVERIFY(written.ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject appearance = reopenObject(built.bytes, written.appearanceObject);
        const PdfObject fonts = entryOf(entryOf(appearance, "Resources"), "Font");
        QVERIFY(fonts.isDictionary());
        QVERIFY2(!entryOf(fonts, "CJK").isDictionary() && !entryOf(fonts, "CJK").isRef(),
                 "純英文註解不該內嵌 CJK 字型");
    }

    void freeTextDictionaryHasDaQAndIt() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeTextBox());
        QVERIFY(written.ok);
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Subtype"), QStringLiteral("FreeText"));
        QVERIFY(hasKey(annot, "DA"));
        QCOMPARE(nameOf(annot, "IT"), QStringLiteral("FreeText"));
        QCOMPARE(entryOf(annot, "Q").asInteger(), std::int64_t{1});  // Center
        QVERIFY(!hasKey(annot, "CL"));  // 不是 Callout，不該有引線
    }

    void typewriterUsesTheTypewriterIntent() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeTypewriter());
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "IT"), QStringLiteral("FreeTextTypewriter"));
    }

    void calloutCarriesClAndLe() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeCallout());
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "IT"), QStringLiteral("FreeTextCallout"));
        const PdfObject cl = entryOf(annot, "CL");
        // start + knee + end = 三個點 = 6 個數字。
        QCOMPARE(arraySize(cl), std::size_t{6});
        QCOMPARE(elementOf(cl, 0).asNumber(), 50.0);
        QCOMPARE(elementOf(cl, 1).asNumber(), 230.0);
        QCOMPARE(elementOf(cl, 4).asNumber(), 5.0);
        QCOMPARE(elementOf(cl, 5).asNumber(), 320.0);
        // FreeText 的 /LE 是單一名稱，不是像 /Line 那樣的兩元素陣列。
        QCOMPARE(nameOf(annot, "LE"), QStringLiteral("OpenArrow"));

        // /Rect 必須把引線終點也包進去。
        const PdfObject rect = entryOf(annot, "Rect");
        QVERIFY(elementOf(rect, 0).asNumber() <= 5.0);   // left
        QVERIFY(elementOf(rect, 3).asNumber() >= 320.0);  // top
    }

    void caretNoneWritesSyNone() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written = writeAnnotation(appender, 0, makeCaret(CaretSymbol::None));
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Subtype"), QStringLiteral("Caret"));
        QCOMPARE(nameOf(annot, "Sy"), QStringLiteral("None"));
        // Caret 不需要字型，/Resources 應保持空字典而不是被硬塞一個 /Font。
        const PdfObject appearance = reopenObject(built.bytes, written.appearanceObject);
        QVERIFY(!hasKey(entryOf(appearance, "Resources"), "Font"));
    }

    void caretParagraphWritesSyP() {
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);
        const AnnotationWriteResult written =
            writeAnnotation(appender, 0, makeCaret(CaretSymbol::Paragraph));
        QVERIFY2(written.ok, written.diagnostic.c_str());
        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        const PdfObject annot = reopenObject(built.bytes, written.annotationObject);
        QCOMPARE(nameOf(annot, "Sy"), QStringLiteral("P"));
    }

    void allSevenAnnotationsTogetherPassQpdfCheck() {
        // WP24 全部七項需求一次寫進同一份檔案，模擬真實審閱情境下多種
        // 註解類型混雜的存檔結果，而不是每種只單獨測、合起來卻沒驗過。
        const std::string source = toStdString(makeFixturePdf());
        IncrementalAppender appender;
        QCOMPARE(appender.open(source), SourceStatus::Ok);

        // PRD-ANN-005 Text Box
        QVERIFY(writeAnnotation(appender, 0, makeTextBox()).ok);
        // PRD-ANN-021 Typewriter
        QVERIFY(writeAnnotation(appender, 0, makeTypewriter()).ok);
        // PRD-ANN-022 Callout（+ PRD-ANN-030 Fit Box 由 app 層在寫入前套用，
        // 這裡直接給已經定好的框，物件層不重複那個計算）
        QVERIFY(writeAnnotation(appender, 0, makeCallout()).ok);
        // PRD-ANN-019 Caret（兩種符號）
        QVERIFY(writeAnnotation(appender, 0, makeCaret(CaretSymbol::None)).ok);
        QVERIFY(writeAnnotation(appender, 0, makeCaret(CaretSymbol::Paragraph)).ok);

        // PRD-ANN-017 Highlight Area／PRD-ANN-018 Free Highlight：兩者都落地成
        // 一般的 /Highlight，這裡各放一則證明同一條物件層通道吃得下。
        Annotation highlightArea;
        highlightArea.color = ColorRgb{1.0, 1.0, 0.0};
        highlightArea.opacity = 0.4;
        TextMarkupGeometry areaGeometry;
        areaGeometry.kind = TextMarkupKind::Highlight;
        areaGeometry.quads.push_back(quadFromPageRect(RectF{10, 10, 190, 40}));
        highlightArea.geometry = areaGeometry;
        QVERIFY(writeAnnotation(appender, 0, highlightArea).ok);

        Annotation freeHighlight;
        freeHighlight.color = ColorRgb{1.0, 1.0, 0.0};
        freeHighlight.opacity = 0.4;
        TextMarkupGeometry strokeGeometry;
        strokeGeometry.kind = TextMarkupKind::Highlight;
        strokeGeometry.quads = ribbonQuadsFromStroke(
            {PointF{10, 60}, PointF{100, 60}, PointF{190, 70}}, 4.0);
        freeHighlight.geometry = strokeGeometry;
        QVERIFY(writeAnnotation(appender, 0, freeHighlight).ok);

        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = QDir::temp().filePath(
            QStringLiteral("alioth-annfamily-%1.pdf").arg(QCoreApplication::applicationPid()));
        const alioth::test::QpdfCheckResult check = runQpdfCheckOnBytes(path, toByteArray(built.bytes));
        if (check.status == QpdfStatus::NotAvailable) {
            QSKIP(qpdfSkipReason().constData());
        }
        QVERIFY2(check.clean(), describeQpdfFailure(QStringLiteral("WP24 混合註解"), check).constData());
    }

    void encryptedDocumentIsRejected() {
        alioth::test::PdfFixtureOptions options;
        options.encrypted = true;
        IncrementalAppender appender;
        QCOMPARE(appender.open(toStdString(makeFixturePdf(options))), SourceStatus::Encrypted);
        QVERIFY(!writeAnnotation(appender, 0, makeTextBox()).ok);
        QVERIFY(!writeAnnotation(appender, 0, makeCaret(CaretSymbol::None)).ok);
    }
};

QTEST_APPLESS_MAIN(TestAnnfamilyObjectWriter)
#include "test_annfamily_object_writer.moc"
