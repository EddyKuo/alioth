// 註解的剪貼簿格式（PRD-ANN-011）。
//
// 驗的是往返與「不亂認」：剪貼簿是全系統共用的，任何程式都能放東西上去，
// 而錯誤地把別人的資料當成註解會直接寫進使用者的 PDF。

#include <QtTest>

#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>

#include "app/annotation_clipboard.h"

using alioth::app::annotationMimeType;
using alioth::app::annotationsFromMimeData;
using alioth::app::makeAnnotationMimeData;
using alioth::app::XfdfEntry;
using alioth::domain::AnnotationType;
using alioth::domain::ColorRgb;
using alioth::domain::quadFromPageRect;
using alioth::domain::RectF;
using alioth::domain::ShapeGeometry;
using alioth::domain::ShapeKind;
using alioth::domain::TextMarkupGeometry;
using alioth::domain::TextMarkupKind;

namespace {

XfdfEntry highlight() {
    XfdfEntry entry;
    entry.pageIndex = 4;
    entry.annotation.author = "審閱者";
    entry.annotation.contents = "這段要再確認";
    entry.annotation.color = ColorRgb{1.0, 0.85, 0.0};
    entry.annotation.opacity = 0.4;
    entry.annotation.geometry = TextMarkupGeometry{
        TextMarkupKind::Highlight, {quadFromPageRect(RectF{20, 100, 120, 116})}};
    return entry;
}

XfdfEntry square() {
    XfdfEntry entry;
    entry.pageIndex = 0;
    entry.annotation.rect = RectF{10, 10, 110, 60};
    entry.annotation.geometry = ShapeGeometry{ShapeKind::Square};
    return entry;
}

}  // namespace

class TestAnnotationClipboard : public QObject {
    Q_OBJECT

private slots:
    void roundTripsThroughTheClipboard();
    void carriesReadablePlainTextAsWell();
    void ignoresClipboardContentThatIsNotOurs();
    void emptySelectionProducesNoMimeData();
};

void TestAnnotationClipboard::roundTripsThroughTheClipboard() {
    const std::vector<XfdfEntry> source{highlight(), square()};
    QMimeData* mime = makeAnnotationMimeData(source, QStringLiteral("review.pdf"));
    QVERIFY(mime != nullptr);
    QGuiApplication::clipboard()->setMimeData(mime);

    const std::vector<XfdfEntry> back =
        annotationsFromMimeData(QGuiApplication::clipboard()->mimeData());
    QCOMPARE(back.size(), std::size_t(2));

    // 頁碼必須各自保留：貼上時要靠它換算跨頁位移，整批共用一個頁碼會讓
    // 所有註解落到同一頁。
    QCOMPARE(back[0].pageIndex, 4);
    QCOMPARE(back[1].pageIndex, 0);
    QCOMPARE(back[0].annotation.type(), AnnotationType::Highlight);
    QCOMPARE(QString::fromStdString(back[0].annotation.author), QStringLiteral("審閱者"));
    QCOMPARE(QString::fromStdString(back[0].annotation.contents), QStringLiteral("這段要再確認"));
    const auto* markup = std::get_if<TextMarkupGeometry>(&back[0].annotation.geometry);
    QVERIFY(markup != nullptr);
    QCOMPARE(markup->quads.size(), std::size_t(1));
    QCOMPARE(back[1].annotation.type(), AnnotationType::Square);
}

void TestAnnotationClipboard::carriesReadablePlainTextAsWell() {
    // 貼到郵件或聊天視窗時應該得到一段可讀的文字，不是一坨 XML。
    QMimeData* mime = makeAnnotationMimeData({highlight()});
    QVERIFY(mime != nullptr);
    QVERIFY(mime->hasText());
    const QString text = mime->text();
    QVERIFY(!text.startsWith(QLatin1Char('<')));
    QVERIFY(text.contains(QStringLiteral("這段要再確認")));
    // 頁碼是 0 起算，顯示要 +1——摘要那一套已經這樣做，這裡沿用同一份。
    QVERIFY(text.contains(QStringLiteral("p.5")));
    delete mime;
}

void TestAnnotationClipboard::ignoresClipboardContentThatIsNotOurs() {
    // 純文字：不去猜內容。
    QGuiApplication::clipboard()->setText(QStringLiteral("just some text"));
    QVERIFY(annotationsFromMimeData(QGuiApplication::clipboard()->mimeData()).empty());

    // 宣稱是我們的型別、內容卻是垃圾：解析失敗就回空，不能半信半疑地
    // 寫一批殘缺的註解進使用者的 PDF。
    auto* fake = new QMimeData;
    fake->setData(annotationMimeType(), QByteArrayLiteral("<not-xfdf/>"));
    QGuiApplication::clipboard()->setMimeData(fake);
    QVERIFY(annotationsFromMimeData(QGuiApplication::clipboard()->mimeData()).empty());

    QVERIFY(annotationsFromMimeData(nullptr).empty());
}

void TestAnnotationClipboard::emptySelectionProducesNoMimeData() {
    // 放一份空的剪貼簿只會讓「貼上」看起來可用，按下去卻什麼都沒發生。
    QVERIFY(makeAnnotationMimeData({}) == nullptr);
}

QTEST_MAIN(TestAnnotationClipboard)
#include "test_annotation_clipboard.moc"
