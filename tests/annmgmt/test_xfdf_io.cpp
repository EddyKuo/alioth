// XFDF 匯入/匯出測試(PRD-ANN-013)。
//
// 重點有二:往返序列化要保住審閱最在意的欄位(頁碼、幾何、顏色、作者、
// 回覆串),以及 XXE 防線要真的擋得住——這裡直接餵 billion-laughs 與
// 外部實體兩種經典攻擊載荷,不是只測「正常輸入能過」。

#include <QtTest>

#include "app/xfdf_io.h"
#include "domain/annotation.h"

using namespace alioth::app;
using namespace alioth::domain;

namespace {

XfdfEntry makeHighlightEntry() {
    XfdfEntry entry;
    entry.pageIndex = 2;
    entry.annotation.id = "alioth-h1";
    entry.annotation.author = "Alice";
    entry.annotation.contents = "請再確認這段";
    entry.annotation.color = ColorRgb{1.0, 1.0, 0.0};
    entry.annotation.opacity = 0.4;
    entry.annotation.geometry =
        TextMarkupGeometry{TextMarkupKind::Highlight, {quadFromPageRect(RectF{10, 100, 150, 120})}};
    return entry;
}

XfdfEntry makeSquareEntry() {
    XfdfEntry entry;
    entry.pageIndex = 0;
    entry.annotation.id = "alioth-s1";
    entry.annotation.color = ColorRgb{1.0, 0.0, 0.0};
    entry.annotation.interiorColor = ColorRgb{0.0, 0.0, 1.0};
    entry.annotation.border.width = 3.0;
    entry.annotation.rect = RectF{5, 5, 55, 45};
    entry.annotation.geometry = ShapeGeometry{ShapeKind::Square};
    return entry;
}

}  // namespace

class TestXfdfIo : public QObject {
    Q_OBJECT

private slots:
    void highlightRoundTripsQuadPointsAndMetadata() {
        const std::vector<XfdfEntry> entries = {makeHighlightEntry()};
        const std::string xml = exportXfdf(entries, "review.pdf");
        QVERIFY(!xml.empty());

        const XfdfImportResult result = importXfdf(xml);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.entries.size(), std::size_t{1});

        const XfdfEntry& back = result.entries.front();
        QCOMPARE(back.pageIndex, 2);
        QCOMPARE(QString::fromStdString(back.annotation.author), QStringLiteral("Alice"));
        QCOMPARE(QString::fromStdString(back.annotation.contents), QStringLiteral("請再確認這段"));
        QCOMPARE(QString::fromStdString(back.annotation.id), QStringLiteral("alioth-h1"));
        QVERIFY(back.annotation.opacity > 0.39 && back.annotation.opacity < 0.41);

        const auto* markup = std::get_if<TextMarkupGeometry>(&back.annotation.geometry);
        QVERIFY(markup != nullptr);
        QCOMPARE(markup->kind, TextMarkupKind::Highlight);
        QCOMPARE(markup->quads.size(), std::size_t{1});
        QVERIFY(std::abs(markup->quads[0].lowerLeft.x - 10.0) < 1e-6);
        QVERIFY(std::abs(markup->quads[0].upperRight.y - 120.0) < 1e-6);
    }

    void squareRoundTripsColorsAndBorderWidth() {
        const std::vector<XfdfEntry> entries = {makeSquareEntry()};
        const XfdfImportResult result = importXfdf(exportXfdf(entries));
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.entries.size(), std::size_t{1});

        const Annotation& back = result.entries.front().annotation;
        QVERIFY(std::get_if<ShapeGeometry>(&back.geometry) != nullptr);
        QVERIFY(back.interiorColor.has_value());
        // 8-bit 色彩量化,允許 1/255 的誤差。
        QVERIFY(std::abs(back.color.r - 1.0) < 0.01);
        QVERIFY(std::abs(back.interiorColor->b - 1.0) < 0.01);
        QVERIFY(std::abs(back.border.width - 3.0) < 0.01);
        QVERIFY(std::abs(back.rect.left - 5.0) < 0.01);
        QVERIFY(std::abs(back.rect.right - 55.0) < 0.01);
    }

    void replyThreadCarriesInReplyTo() {
        XfdfEntry parent = makeHighlightEntry();
        XfdfEntry reply = makeHighlightEntry();
        reply.annotation.id = "alioth-h2";
        reply.annotation.inReplyTo = parent.annotation.id;

        const XfdfImportResult result = importXfdf(exportXfdf({parent, reply}));
        QVERIFY(result.ok);
        QCOMPARE(result.entries.size(), std::size_t{2});
        QVERIFY(result.entries[1].annotation.inReplyTo.has_value());
        QCOMPARE(QString::fromStdString(*result.entries[1].annotation.inReplyTo),
                 QStringLiteral("alioth-h1"));
    }

    // PRD-ANN-013 安全要求:XML 實體展開必須擋掉。
    void billionLaughsPayloadIsRejected() {
        const std::string payload =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE xfdf [\n"
            "<!ENTITY lol \"lol\">\n"
            "<!ENTITY lol2 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">\n"
            "<!ENTITY lol3 \"&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;\">\n"
            "]>\n"
            "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\"><annots>"
            "<highlight page=\"0\"><contents>&lol3;</contents></highlight>"
            "</annots></xfdf>";

        const XfdfImportResult result = importXfdf(payload);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.entries.empty());
    }

    void externalEntityPayloadIsRejected() {
        const std::string payload =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE xfdf [\n"
            "<!ENTITY xxe SYSTEM \"file:///etc/passwd\">\n"
            "]>\n"
            "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\"><annots>"
            "<highlight page=\"0\"><contents>&xxe;</contents></highlight>"
            "</annots></xfdf>";

        const XfdfImportResult result = importXfdf(payload);
        QVERIFY(!result.ok);
        QVERIFY(result.diagnostic.find("DOCTYPE") != std::string::npos);
    }

    void oversizedInputIsRejected() {
        std::string huge(11 * 1024 * 1024, 'x');
        const XfdfImportResult result = importXfdf(huge);
        QVERIFY(!result.ok);
    }

    void unsupportedElementsAreSkippedNotFatal() {
        const std::string xml =
            "<?xml version=\"1.0\"?>"
            "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\"><annots>"
            "<highlight page=\"0\" rect=\"0,0,10,10\"><quadpoints>0,10,10,10,0,0,10,0</quadpoints></highlight>"
            "<fileattachment page=\"0\"/>"
            "</annots></xfdf>";
        const XfdfImportResult result = importXfdf(xml);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.entries.size(), std::size_t{1});
        QVERIFY(!result.skippedElements.empty());
    }
};

QTEST_APPLESS_MAIN(TestXfdfIo)
#include "test_xfdf_io.moc"
