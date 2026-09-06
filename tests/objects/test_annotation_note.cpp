// 改寫既有註解的註釋文字（PRD-ANN-004 的彈出視窗編輯）。
//
// 驗的是三件事：改得動、清得掉、以及**該拒絕的要拒絕**。第三件才是這支測試
// 存在的主要理由——/FreeText 的外觀串流畫的就是 /Contents，只改字典會讓
// Acrobat（信任 /AP）與其他自行繪製的檢視器顯示不同的文字，而那種分岔在
// 開發機上不會被看見。

#include <QtTest>

#include <optional>
#include <string>
#include <variant>

#include "engine/objects/annotation_note_writer.h"
#include "engine/objects/annotation_object_writer.h"
#include "engine/objects/incremental_appender.h"
#include "object_fixture.h"

using namespace alioth::engine::objects;
using alioth::domain::Annotation;
using alioth::domain::ColorRgb;
using alioth::domain::FreeTextGeometry;
using alioth::domain::RectF;
using alioth::domain::TextNoteGeometry;
using alioth::test::makeFixturePdf;
using alioth::test::toStdString;

namespace {

Annotation makeStickyNote(const std::string& contents) {
    Annotation annotation{};
    annotation.author = "審閱者";
    annotation.contents = contents;
    annotation.color = ColorRgb{1.0, 0.85, 0.0};
    annotation.rect = RectF{40, 700, 60, 720};
    annotation.geometry = TextNoteGeometry{};
    return annotation;
}

// 讀回某個物件的 /Contents（UTF-8）。找不到鍵時回傳 std::nullopt 以區分
// 「清空了」與「值是空字串」——後者在 PDF 裡是兩種不同的狀態。
std::optional<std::string> contentsOf(const std::string& bytes, int objectNumber) {
    IncrementalAppender appender;
    if (appender.open(bytes) != SourceStatus::Ok) return std::nullopt;
    const PdfObject object = appender.currentObject(objectNumber);
    const PdfDictionary* dict = object.asDictionary();
    if (dict == nullptr) return std::nullopt;
    const PdfObject* value = dict->find("Contents");
    if (value == nullptr) return std::nullopt;
    const auto* text = std::get_if<PdfString>(&value->value());
    if (text == nullptr) return std::nullopt;
    return text->bytes;
}

// PDF 文字字串（ISO 32000 §7.9.2.2）：非 ASCII 一律以 UTF-16BE 加 BOM 儲存，
// 這是 makeTextString() 的行為。測試要比對的是使用者看到的字，不是編碼結果，
// 所以在這裡解回來——直接比對位元組會讓一支正確的實作看起來像壞掉。
QString decodePdfText(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        QString out;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const auto high = static_cast<unsigned char>(bytes[i]);
            const auto low = static_cast<unsigned char>(bytes[i + 1]);
            out.append(QChar(static_cast<char16_t>((high << 8) | low)));
        }
        return out;
    }
    return QString::fromLatin1(bytes.c_str(), static_cast<int>(bytes.size()));
}

}  // namespace

class TestAnnotationNote : public QObject {
    Q_OBJECT

private slots:
    void rewritesContentsAndBumpsModifiedDate();
    void emptyContentsRemovesTheKey();
    void rejectsFreeTextBecauseItsAppearanceDrawsTheText();
    void rejectsPopupAndNonAnnotationObjects();
    void keepsGeometryAndAppearanceUntouched();
};

void TestAnnotationNote::rewritesContentsAndBumpsModifiedDate() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);
    const AnnotationWriteResult written = writeAnnotation(appender, 0, makeStickyNote("原本的字"));
    QVERIFY(written.ok);

    const NoteEditResult edited = setAnnotationContents(appender, written.annotationObject,
                                                        "改過的字", "D:20260906120000+08'00'");
    QVERIFY2(edited.ok, edited.diagnostic.c_str());

    const BuildResult built = appender.build();
    QVERIFY(built.ok);
    const std::string bytes(reinterpret_cast<const char*>(built.bytes.data()), built.bytes.size());

    const auto contents = contentsOf(bytes, written.annotationObject);
    QVERIFY(contents.has_value());
    QCOMPARE(decodePdfText(*contents), QStringLiteral("改過的字"));

    IncrementalAppender reader;
    QCOMPARE(reader.open(bytes), SourceStatus::Ok);
    const PdfObject object = reader.currentObject(written.annotationObject);
    const PdfDictionary* dict = object.asDictionary();
    QVERIFY(dict != nullptr);
    const PdfObject* modified = dict->find("M");
    QVERIFY(modified != nullptr);
    const auto* modifiedText = std::get_if<PdfString>(&modified->value());
    QVERIFY(modifiedText != nullptr);
    QCOMPARE(decodePdfText(modifiedText->bytes),
             QStringLiteral("D:20260906120000+08'00'"));
}

void TestAnnotationNote::emptyContentsRemovesTheKey() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);
    const AnnotationWriteResult written = writeAnnotation(appender, 0, makeStickyNote("有字"));
    QVERIFY(written.ok);

    QVERIFY(setAnnotationContents(appender, written.annotationObject, "", "").ok);

    const BuildResult built = appender.build();
    QVERIFY(built.ok);
    const std::string bytes(reinterpret_cast<const char*>(built.bytes.data()), built.bytes.size());
    QVERIFY(!contentsOf(bytes, written.annotationObject).has_value());
}

void TestAnnotationNote::rejectsFreeTextBecauseItsAppearanceDrawsTheText() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);

    Annotation annotation{};
    annotation.contents = "ASCII only";
    annotation.color = ColorRgb{0.0, 0.0, 0.0};
    annotation.rect = RectF{40, 600, 240, 660};
    FreeTextGeometry freeText;
    freeText.text = "ASCII only";
    annotation.geometry = freeText;
    const AnnotationWriteResult written = writeAnnotation(appender, 0, annotation);
    QVERIFY2(written.ok, written.diagnostic.c_str());

    const NoteEditResult edited =
        setAnnotationContents(appender, written.annotationObject, "changed", "");
    QVERIFY(!edited.ok);
    QVERIFY(!edited.diagnostic.empty());
}

void TestAnnotationNote::rejectsPopupAndNonAnnotationObjects() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);
    const AnnotationWriteResult written = writeAnnotation(appender, 0, makeStickyNote("有字"));
    QVERIFY(written.ok);
    QVERIFY(written.popupObject != 0);

    // /Popup 沒有註釋文字，寫進去只會讓「這一頁有幾則註解」與內容對不上。
    QVERIFY(!setAnnotationContents(appender, written.popupObject, "x", "").ok);
    // 外觀串流是 Form XObject，不是註解字典；它沒有 /Subtype Annot 的語意。
    QVERIFY(!setAnnotationContents(appender, written.appearanceObject, "x", "").ok);
    // 完全不存在的編號。
    QVERIFY(!setAnnotationContents(appender, 99999, "x", "").ok);
}

void TestAnnotationNote::keepsGeometryAndAppearanceUntouched() {
    const std::string source = toStdString(makeFixturePdf());
    IncrementalAppender appender;
    QCOMPARE(appender.open(source), SourceStatus::Ok);
    const AnnotationWriteResult written = writeAnnotation(appender, 0, makeStickyNote("原本"));
    QVERIFY(written.ok);

    const PdfObject before = appender.currentObject(written.annotationObject);
    const PdfDictionary* beforeDict = before.asDictionary();
    QVERIFY(beforeDict != nullptr);
    const PdfObject beforeRect = *beforeDict->find("Rect");
    const PdfObject beforeAp = *beforeDict->find("AP");

    QVERIFY(setAnnotationContents(appender, written.annotationObject, "改過", "").ok);

    const PdfObject after = appender.currentObject(written.annotationObject);
    const PdfDictionary* afterDict = after.asDictionary();
    QVERIFY(afterDict != nullptr);
    // 便利貼的外觀是一個固定圖示，與註釋文字無關——改字不該碰幾何或 /AP，
    // 否則「編輯註釋」會悄悄變成「重畫這則註解」。
    QCOMPARE(serialize(*afterDict->find("Rect")), serialize(beforeRect));
    QCOMPARE(serialize(*afterDict->find("AP")), serialize(beforeAp));
}

QTEST_APPLESS_MAIN(TestAnnotationNote)
#include "test_annotation_note.moc"
