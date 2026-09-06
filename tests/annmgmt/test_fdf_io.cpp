// FDF 匯入／匯出（PRD-ANN-013）。
//
// 重點在往返：匯出再匯入必須拿回同一批註解。FDF 的欄位散在幾何、顏色、日期
// 三處，只驗「能解析」會讓一個把 /QuadPoints 少寫兩個數字的實作照樣通過。
//
// 另一半重點是**不可信任輸入**：FDF 來自信件附件，畸形檔案是常態。壞掉的
// 檔案要明確失敗或明確跳過，不能靜默漏收，也不能把整批註解一起丟掉。

#include <QtTest>

#include <string>

#include "app/fdf_io.h"

using alioth::app::exportFdf;
using alioth::app::FdfEntry;
using alioth::app::importFdf;
using alioth::domain::AnnotationType;
using alioth::domain::ColorRgb;
using alioth::domain::InkGeometry;
using alioth::domain::LineGeometry;
using alioth::domain::PdfDate;
using alioth::domain::PointF;
using alioth::domain::PolygonGeometry;
using alioth::domain::quadFromPageRect;
using alioth::domain::RectF;
using alioth::domain::ShapeGeometry;
using alioth::domain::ShapeKind;
using alioth::domain::TextMarkupGeometry;
using alioth::domain::TextMarkupKind;
using alioth::domain::TextNoteGeometry;

namespace {

FdfEntry highlight() {
    FdfEntry entry;
    entry.pageIndex = 3;
    entry.annotation.id = "alioth-1";
    entry.annotation.author = "審閱者";
    entry.annotation.contents = "這段要再確認";
    entry.annotation.subject = "疑問";
    entry.annotation.color = ColorRgb{1.0, 0.85, 0.0};
    entry.annotation.opacity = 0.4;
    entry.annotation.creationDate = PdfDate{2026, 9, 6, 10, 15, 0, 8, 0};
    entry.annotation.modifiedDate = entry.annotation.creationDate;
    entry.annotation.geometry =
        TextMarkupGeometry{TextMarkupKind::Highlight,
                           {quadFromPageRect(RectF{20, 100, 120, 116}),
                            quadFromPageRect(RectF{20, 80, 90, 96})}};
    return entry;
}

FdfEntry square() {
    FdfEntry entry;
    entry.pageIndex = 0;
    entry.annotation.rect = RectF{10, 10, 110, 60};
    entry.annotation.color = ColorRgb{0.0, 0.0, 1.0};
    entry.annotation.interiorColor = ColorRgb{0.9, 0.9, 0.9};
    entry.annotation.border.width = 2.5;
    entry.annotation.geometry = ShapeGeometry{ShapeKind::Square};
    return entry;
}

FdfEntry ink() {
    FdfEntry entry;
    entry.pageIndex = 1;
    entry.annotation.rect = RectF{0, 0, 200, 200};
    entry.annotation.geometry =
        InkGeometry{{{PointF{10, 10}, PointF{20, 30}, PointF{40, 25}}, {PointF{60, 60}}}};
    return entry;
}

}  // namespace

class TestFdfIo : public QObject {
    Q_OBJECT

private slots:
    void roundTripsTheSupportedSubset();
    void keepsPageNumbersPerAnnotation();
    void rejectsInputThatIsNotFdf();
    void rejectsOversizedInput();
    void reportsJavaScriptInsteadOfSilentlyDroppingIt();
    void skipsUnsupportedSubtypesWithoutLosingTheRest();
    void survivesMalformedObjectsInTheBody();
};

void TestFdfIo::roundTripsTheSupportedSubset() {
    const std::vector<FdfEntry> entries{highlight(), square(), ink()};
    const std::string bytes = exportFdf(entries, "review.pdf");
    QVERIFY(bytes.rfind("%FDF-", 0) == 0);

    const auto result = importFdf(bytes);
    QVERIFY2(result.ok, result.diagnostic.c_str());
    QVERIFY(result.skipped.empty());
    QCOMPARE(result.entries.size(), entries.size());

    const auto& back = result.entries[0].annotation;
    QCOMPARE(back.type(), AnnotationType::Highlight);
    QCOMPARE(QString::fromStdString(back.author), QStringLiteral("審閱者"));
    QCOMPARE(QString::fromStdString(back.contents), QStringLiteral("這段要再確認"));
    QCOMPARE(QString::fromStdString(back.subject), QStringLiteral("疑問"));
    QCOMPARE(back.id, std::string("alioth-1"));
    QVERIFY(qFuzzyCompare(back.opacity, 0.4));
    QVERIFY(qFuzzyCompare(back.color.g, 0.85));
    // 時區以外的欄位必須完全一致（見 domain::fromPdfDateString 的說明）。
    QCOMPARE(back.creationDate.year, 2026);
    QCOMPARE(back.creationDate.month, 9);
    QCOMPARE(back.creationDate.day, 6);
    QCOMPARE(back.creationDate.hour, 10);
    QCOMPARE(back.creationDate.minute, 15);
    const auto* markup = std::get_if<TextMarkupGeometry>(&back.geometry);
    QVERIFY(markup != nullptr);
    // 兩組 quad，共 16 個數字。少寫兩個就會在這裡變成一組。
    QCOMPARE(markup->quads.size(), std::size_t(2));
    QVERIFY(qFuzzyCompare(markup->quads[0].upperLeft.x, 20.0));

    const auto& shape = result.entries[1].annotation;
    QCOMPARE(shape.type(), AnnotationType::Square);
    QVERIFY(shape.interiorColor.has_value());
    QVERIFY(qFuzzyCompare(shape.border.width, 2.5));
    QVERIFY(qFuzzyCompare(shape.rect.right, 110.0));

    const auto* strokes = std::get_if<InkGeometry>(&result.entries[2].annotation.geometry);
    QVERIFY(strokes != nullptr);
    QCOMPARE(strokes->strokes.size(), std::size_t(2));
    QCOMPARE(strokes->strokes[0].size(), std::size_t(3));
    QVERIFY(qFuzzyCompare(strokes->strokes[0][1].y, 30.0));
}

void TestFdfIo::keepsPageNumbersPerAnnotation() {
    // FDF 的頁碼是每則註解自帶的，不是整批共用——弄錯的話所有註解都會
    // 落到同一頁，而那在只有一頁的測試文件上完全看不出來。
    const std::string bytes = exportFdf({highlight(), square(), ink()});
    const auto result = importFdf(bytes);
    QVERIFY(result.ok);
    QCOMPARE(result.entries.size(), std::size_t(3));
    QCOMPARE(result.entries[0].pageIndex, 3);
    QCOMPARE(result.entries[1].pageIndex, 0);
    QCOMPARE(result.entries[2].pageIndex, 1);
}

void TestFdfIo::rejectsInputThatIsNotFdf() {
    const auto result = importFdf("%PDF-1.7\n1 0 obj\n<< >>\nendobj\n");
    QVERIFY(!result.ok);
    QVERIFY(!result.diagnostic.empty());
    QVERIFY(result.entries.empty());
}

void TestFdfIo::rejectsOversizedInput() {
    std::string huge = "%FDF-1.2\n";
    huge.resize(alioth::app::kMaxFdfBytes + 1, ' ');
    const auto result = importFdf(huge);
    QVERIFY(!result.ok);
    QVERIFY(result.entries.empty());
}

void TestFdfIo::reportsJavaScriptInsteadOfSilentlyDroppingIt() {
    // FDF 合法地可以帶 /JavaScript。本產品不執行它，但要說出來——
    // 靜默丟掉會讓「為什麼對方的自動計算沒作用」變成無從查起的問題。
    const std::string bytes =
        "%FDF-1.2\n"
        "1 0 obj\n<< /FDF << /JavaScript << /Before (app.alert\\(1\\)) >> /Annots [] >> >>\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
    const auto result = importFdf(bytes);
    QVERIFY(result.ok);
    QVERIFY(result.entries.empty());
    QCOMPARE(result.skipped.size(), std::size_t(1));
    QVERIFY(result.skipped[0].find("JavaScript") != std::string::npos);
}

void TestFdfIo::skipsUnsupportedSubtypesWithoutLosingTheRest() {
    const std::string bytes =
        "%FDF-1.2\n"
        "1 0 obj\n<< /FDF << /Annots [ 2 0 R 3 0 R 4 0 R ] >> >>\nendobj\n"
        "2 0 obj\n<< /Type /Annot /Subtype /Widget /Page 0 /Rect [0 0 1 1] >>\nendobj\n"
        "3 0 obj\n<< /Type /Annot /Subtype /Text /Page 2 /Rect [10 20 30 40] "
        "/Contents (hello) >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Page 0 /Rect [0 0 1 1] >>\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
    const auto result = importFdf(bytes);
    QVERIFY(result.ok);
    // 支援的那一則必須進來，不支援的兩則必須被列出來而不是消失。
    QCOMPARE(result.entries.size(), std::size_t(1));
    QCOMPARE(result.entries[0].pageIndex, 2);
    QCOMPARE(result.entries[0].annotation.type(), AnnotationType::Text);
    QCOMPARE(QString::fromStdString(result.entries[0].annotation.contents),
             QStringLiteral("hello"));
    QCOMPARE(result.skipped.size(), std::size_t(2));
}

void TestFdfIo::survivesMalformedObjectsInTheBody() {
    // 中間夾一段垃圾。掃描式的剖析要能跳過它繼續往下讀，
    // 而不是在第一個非物件位元組就放棄整份檔案。
    const std::string bytes =
        "%FDF-1.2\n"
        "%% 這行是註解\n"
        "1 0 obj\n<< /FDF << /Annots [ 2 0 R ] >> >>\nendobj\n"
        "!!! 這裡不是物件 !!!\n"
        "2 0 obj\n<< /Type /Annot /Subtype /Square /Page 5 /Rect [1 2 3 4] >>\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
    const auto result = importFdf(bytes);
    QVERIFY2(result.ok, result.diagnostic.c_str());
    QCOMPARE(result.entries.size(), std::size_t(1));
    QCOMPARE(result.entries[0].pageIndex, 5);
    QCOMPARE(result.entries[0].annotation.type(), AnnotationType::Square);
}

QTEST_MAIN(TestFdfIo)
#include "test_fdf_io.moc"
