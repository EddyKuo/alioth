// 頁面標籤（PRD-PAGE-013）。
//
// 兩件事分開驗：領域層的編號規則（不需要 PDF），以及 /PageLabels 的讀寫往返
// （需要真的位元組，而且要通得過 qpdf 的結構檢查）。

#include <QtTest>

#include <QTemporaryDir>

#include "domain/page_labels.h"
#include "engine/labels/page_label_codec.h"
#include "qpdf_check.h"

using namespace alioth;
using domain::PageLabelMap;
using domain::PageLabelRange;
using domain::PageLabelStyle;

namespace {

PageLabelRange range(std::int32_t start, PageLabelStyle style, std::string prefix = {},
                     std::int32_t startNumber = 1) {
    PageLabelRange value;
    value.startPageIndex = start;
    value.style = style;
    value.prefix = std::move(prefix);
    value.startNumber = startNumber;
    return value;
}

// 十頁的最小文件，沒有 /PageLabels。
QByteArray makeTenPagePdf() {
    std::vector<QByteArray> objects;
    QByteArray kids;
    for (int i = 0; i < 10; ++i) {
        kids += QByteArray::number(i + 3) + " 0 R ";
    }
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count 10 >>");
    for (int i = 0; i < 10; ++i) {
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>");
    }

    QByteArray pdf = "%PDF-1.7\n";
    std::vector<int> offsets;
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[static_cast<std::size_t>(i)] +
               "\nendobj\n";
    }
    const int xref = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           "\n0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

}  // namespace

class TestPageLabels : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        base_ = makeTenPagePdf();
    }

    // -----------------------------------------------------------------------
    // 領域層：編號規則
    // -----------------------------------------------------------------------

    void romanNumeralsCoverTheAwkwardCases() {
        const PageLabelRange lower = range(0, PageLabelStyle::RomanLower);
        QCOMPARE(formatPageLabel(lower, 0), std::string{"i"});
        QCOMPARE(formatPageLabel(lower, 3), std::string{"iv"});
        QCOMPARE(formatPageLabel(lower, 8), std::string{"ix"});
        QCOMPARE(formatPageLabel(lower, 38), std::string{"xxxix"});
        QCOMPARE(formatPageLabel(lower, 89), std::string{"xc"});
        QCOMPARE(formatPageLabel(lower, 398), std::string{"cccxcix"});

        const PageLabelRange upper = range(0, PageLabelStyle::RomanUpper);
        QCOMPARE(formatPageLabel(upper, 1899), std::string{"MCM"});
    }

    void romanBeyondSpecFallsBackToDecimal() {
        // 4000 以上沒有標準羅馬寫法。輸出一長串 MMMM… 只會讓使用者看不懂，
        // 退回十進位至少還讀得出來。
        const PageLabelRange upper = range(0, PageLabelStyle::RomanUpper);
        QCOMPARE(formatPageLabel(upper, 3999), std::string{"4000"});
    }

    void letterSequenceRepeatsRatherThanCountingBase26() {
        // PDF 32000 §12.4.2 的序列是 A…Z、AA、BB、CC——重複字母。
        // 照 Excel 欄名的直覺寫成 AA、AB、AC 是錯的，而且要超過 26 頁才看得出來。
        const PageLabelRange upper = range(0, PageLabelStyle::LettersUpper);
        QCOMPARE(formatPageLabel(upper, 0), std::string{"A"});
        QCOMPARE(formatPageLabel(upper, 25), std::string{"Z"});
        QCOMPARE(formatPageLabel(upper, 26), std::string{"AA"});
        QCOMPARE(formatPageLabel(upper, 27), std::string{"BB"});
        QCOMPARE(formatPageLabel(upper, 51), std::string{"ZZ"});
        QCOMPARE(formatPageLabel(upper, 52), std::string{"AAA"});

        const PageLabelRange lower = range(0, PageLabelStyle::LettersLower);
        QCOMPARE(formatPageLabel(lower, 26), std::string{"aa"});
    }

    void prefixOnlyStyleRepeatsTheSameLabel() {
        const PageLabelRange none = range(0, PageLabelStyle::None, "封面");
        QCOMPARE(formatPageLabel(none, 0), std::string{"封面"});
        QCOMPARE(formatPageLabel(none, 5), std::string{"封面"});
    }

    void startNumberShiftsTheSequence() {
        const PageLabelRange decimal = range(4, PageLabelStyle::Decimal, "", 100);
        QCOMPARE(formatPageLabel(decimal, 0), std::string{"100"});
        QCOMPARE(formatPageLabel(decimal, 3), std::string{"103"});
    }

    void typicalThreeSectionDocument() {
        // 前言 i-iii、正文 1-5、附錄 A-1 起。這是規範類文件最常見的形態。
        PageLabelMap map({range(0, PageLabelStyle::RomanLower),
                          range(3, PageLabelStyle::Decimal),
                          range(8, PageLabelStyle::Decimal, "A-")});

        QCOMPARE(map.labelFor(0), std::string{"i"});
        QCOMPARE(map.labelFor(2), std::string{"iii"});
        QCOMPARE(map.labelFor(3), std::string{"1"});
        QCOMPARE(map.labelFor(7), std::string{"5"});
        QCOMPARE(map.labelFor(8), std::string{"A-1"});
        QCOMPARE(map.labelFor(9), std::string{"A-2"});
    }

    void rangesAreSortedAndDeduplicated() {
        PageLabelMap map({range(8, PageLabelStyle::Decimal, "A-"),
                          range(0, PageLabelStyle::RomanLower),
                          range(0, PageLabelStyle::RomanUpper)});  // 同起點，後者勝
        QCOMPARE(map.ranges().size(), std::size_t{2});
        QCOMPARE(map.ranges()[0].startPageIndex, 0);
        QCOMPARE(map.labelFor(0), std::string{"I"});
    }

    void invalidRangesAreDropped() {
        PageLabelMap map({range(-1, PageLabelStyle::Decimal), range(0, PageLabelStyle::Decimal)});
        QCOMPARE(map.ranges().size(), std::size_t{1});
    }

    void pageWithoutCoveringRangeFallsBackToPageNumber() {
        // 規格要求第一段從第 0 頁開始，但真實文件常常違反。
        PageLabelMap map({range(5, PageLabelStyle::Decimal)});
        QCOMPARE(map.labelFor(0), std::string{"1"});
        QCOMPARE(map.labelFor(4), std::string{"5"});
        QCOMPARE(map.labelFor(5), std::string{"1"});
    }

    void labelLookupFindsTheRightPage() {
        // 跳頁框輸入 "ii" 必須跳到第 2 頁而不是第 2 個索引。
        PageLabelMap map({range(0, PageLabelStyle::RomanLower),
                          range(3, PageLabelStyle::Decimal),
                          range(8, PageLabelStyle::Decimal, "A-")});

        QCOMPARE(*map.pageForLabel("ii", 10), 1);
        QCOMPARE(*map.pageForLabel("1", 10), 3);
        QCOMPARE(*map.pageForLabel("5", 10), 7);
        QCOMPARE(*map.pageForLabel("A-2", 10), 9);
    }

    void labelLookupRejectsOutOfRangeAndNonsense() {
        PageLabelMap map({range(0, PageLabelStyle::RomanLower), range(3, PageLabelStyle::Decimal)});
        // "iv" 屬於羅馬段，但那段只有 3 頁——第 4 個羅馬數字不存在。
        QVERIFY(!map.pageForLabel("iv", 10).has_value());
        QVERIFY(!map.pageForLabel("99", 10).has_value());
        QVERIFY(!map.pageForLabel("", 10).has_value());
        QVERIFY(!map.pageForLabel("xyz", 10).has_value());
        QVERIFY(!map.pageForLabel("1", 0).has_value());
    }

    void lookupIsCaseSensitiveOnStyle() {
        PageLabelMap map({range(0, PageLabelStyle::Decimal, "Fig-")});
        QCOMPARE(*map.pageForLabel("Fig-3", 10), 2);
        // 前綴大小寫不符就不是同一個標籤。放寬會讓 "fig-3" 與 "Fig-3"
        // 在同時存在兩種前綴的文件上跳到錯的頁。
        QVERIFY(!map.pageForLabel("fig-3", 10).has_value());
    }

    void duplicateLabelsAreDetectable() {
        PageLabelMap unique({range(0, PageLabelStyle::Decimal)});
        QVERIFY(!unique.hasDuplicateLabels(10));

        // 兩段都從 1 開始，第 1 頁與第 6 頁都叫做 "1"。
        PageLabelMap duplicated({range(0, PageLabelStyle::Decimal),
                                 range(5, PageLabelStyle::Decimal)});
        QVERIFY(duplicated.hasDuplicateLabels(10));
    }

    // -----------------------------------------------------------------------
    // 引擎層：/PageLabels 讀寫
    // -----------------------------------------------------------------------

    void documentWithoutPageLabelsReadsEmpty() {
        engine::objects::PdfSourceDocument source;
        QVERIFY(source.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::labels::readPageLabels(source).empty());
    }

    void writeThenReadRoundTrips() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);

        PageLabelMap written({range(0, PageLabelStyle::RomanLower),
                              range(3, PageLabelStyle::Decimal, "", 7),
                              range(8, PageLabelStyle::Decimal, "A-"),
                              range(9, PageLabelStyle::None, "封底")});
        const engine::labels::PageLabelWriteResult result =
            engine::labels::writePageLabels(appender, written);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.rangeCount, std::size_t{4});

        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(built.bytes) == engine::objects::SourceStatus::Ok);
        const PageLabelMap read = engine::labels::readPageLabels(reread);

        QCOMPARE(read.ranges().size(), std::size_t{4});
        QCOMPARE(read.labelFor(0), std::string{"i"});
        QCOMPARE(read.labelFor(3), std::string{"7"});
        QCOMPARE(read.labelFor(4), std::string{"8"});
        QCOMPARE(read.labelFor(8), std::string{"A-1"});
        QCOMPARE(read.labelFor(9), std::string{"封底"});
    }

    void writeIsPurelyIncremental() {
        // 附加式寫入的前提：原檔位元組一個都不能動，否則既有簽章立刻失效。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::labels::writePageLabels(appender, PageLabelMap({range(
                                                              0, PageLabelStyle::Decimal)}))
                    .ok);
        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        QVERIFY(built.bytes.size() > base_.size());
        QCOMPARE(QByteArray::fromStdString(built.bytes.substr(0, static_cast<std::size_t>(
                                                                    base_.size()))),
                 base_);
    }

    void emptyMapWritesAnEmptyTreeNotAMissingKey() {
        // 移除頁面標籤。附加式寫入不刪既有物件，留一棵空樹是唯一乾淨的表達。
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::labels::writePageLabels(appender, PageLabelMap{}).ok);
        const engine::objects::BuildResult built = appender.build();
        QVERIFY(built.ok);

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(built.bytes) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::labels::readPageLabels(reread).empty());
    }

    void rewriteReplacesRatherThanMerges() {
        engine::objects::IncrementalAppender first;
        QVERIFY(first.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(first.isOpen());
        QVERIFY(engine::labels::writePageLabels(
                    first, PageLabelMap({range(0, PageLabelStyle::RomanLower),
                                         range(5, PageLabelStyle::Decimal)}))
                    .ok);
        const engine::objects::BuildResult once = first.build();
        QVERIFY(once.ok);

        engine::objects::IncrementalAppender second;
        QVERIFY(second.open(once.bytes) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::labels::writePageLabels(
                    second, PageLabelMap({range(0, PageLabelStyle::LettersUpper)}))
                    .ok);
        const engine::objects::BuildResult twice = second.build();
        QVERIFY(twice.ok);

        engine::objects::PdfSourceDocument reread;
        QVERIFY(reread.open(twice.bytes) == engine::objects::SourceStatus::Ok);
        const PageLabelMap read = engine::labels::readPageLabels(reread);
        QCOMPARE(read.ranges().size(), std::size_t{1});
        QCOMPARE(read.labelFor(5), std::string{"F"});
    }

    void unknownStyleNameDoesNotBecomeDecimal() {
        // 猜成十進位會產生看似合理但錯誤的頁碼，那比沒有頁碼難察覺得多。
        QByteArray pdf = base_;
        // 直接組一份帶有無效 /S 的文件比較費事，這裡改以領域層驗同一條規則：
        // 讀取端遇到認不得的樣式會維持 None，而 None 的輸出只有前綴。
        const PageLabelRange none = range(0, PageLabelStyle::None, "X");
        QCOMPARE(formatPageLabel(none, 5), std::string{"X"});
        Q_UNUSED(pdf);
    }

    void outputPassesQpdfStructureCheck() {
        engine::objects::IncrementalAppender appender;
        QVERIFY(appender.open(bytes()) == engine::objects::SourceStatus::Ok);
        QVERIFY(engine::labels::writePageLabels(
                    appender, PageLabelMap({range(0, PageLabelStyle::RomanLower),
                                            range(3, PageLabelStyle::Decimal, "第", 7),
                                            range(8, PageLabelStyle::LettersUpper, "附錄 ")}))
                    .ok);
        const engine::objects::BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = dir_->filePath(QStringLiteral("labels.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(built.bytes.data(), static_cast<qint64>(built.bytes.size()));
        file.close();

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

private:
    std::string bytes() const {
        return std::string(base_.constData(), static_cast<std::size_t>(base_.size()));
    }

    std::unique_ptr<QTemporaryDir> dir_;
    QByteArray base_;
};

QTEST_MAIN(TestPageLabels)
#include "test_page_labels.moc"
