// 無障礙檢查器測試（PRD-A11Y-003）。

#include <QtTest>

#include <algorithm>

#include "a11y_checker_fixture.h"
#include "engine/objects/accessibility_checker.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/objects/struct_tree_reader.h"

using namespace alioth::engine::objects;

namespace {

[[nodiscard]] bool hasFinding(const A11yReport& report, A11yCheckId check) {
    return std::any_of(report.findings.begin(), report.findings.end(),
                       [check](const A11yFinding& f) { return f.check == check; });
}

[[nodiscard]] const A11yFinding* findFinding(const A11yReport& report, A11yCheckId check) {
    const auto it = std::find_if(report.findings.begin(), report.findings.end(),
                                 [check](const A11yFinding& f) { return f.check == check; });
    return it == report.findings.end() ? nullptr : &*it;
}

}  // namespace

class TestAccessibilityChecker : public QObject {
    Q_OBJECT

private slots:
    void detectsAllSeededDefects() {
        const QByteArray pdf = alioth::test::makeDefectivePdf();
        PdfSourceDocument source;
        QCOMPARE(static_cast<int>(source.open(alioth::test::toStdString(pdf))),
                 static_cast<int>(SourceStatus::Ok));
        const StructTree tree = readStructTree(source);
        QCOMPARE(static_cast<int>(tree.status), static_cast<int>(StructTreeStatus::Ok));

        const A11yReport report = runAccessibilityCheck(source, tree);

        QVERIFY(hasFinding(report, A11yCheckId::DocumentTitle));
        QVERIFY(hasFinding(report, A11yCheckId::DocumentLanguage));
        QVERIFY(!hasFinding(report, A11yCheckId::TagStructure));  // 有標籤且已標記，不該誤報

        const A11yFinding* alt = findFinding(report, A11yCheckId::ImageAltText);
        QVERIFY(alt != nullptr);
        QCOMPARE(alt->pageIndex, 0);
        QVERIFY(QString::fromStdString(alt->element).contains(QStringLiteral("10")));

        const A11yFinding* table = findFinding(report, A11yCheckId::TableHeaders);
        QVERIFY(table != nullptr);
        QCOMPARE(table->pageIndex, 0);

        const A11yFinding* heading = findFinding(report, A11yCheckId::HeadingHierarchy);
        QVERIFY(heading != nullptr);
        QVERIFY(QString::fromStdString(heading->reason).contains(QStringLiteral("H1")));
        QVERIFY(QString::fromStdString(heading->reason).contains(QStringLiteral("H3")));

        const A11yFinding* contrast = findFinding(report, A11yCheckId::Contrast);
        QVERIFY(contrast != nullptr);
        QCOMPARE(contrast->pageIndex, 0);

        // 每一種檢查（不論本輪支不支援）都要出現在涵蓋清單，這樣「沒有發現」與
        // 「這項根本沒有跑」才能被使用者分辨。
        const std::vector<A11yCheckId> allChecks = {
            A11yCheckId::DocumentTitle,   A11yCheckId::DocumentLanguage,
            A11yCheckId::TagStructure,    A11yCheckId::ImageAltText,
            A11yCheckId::TableHeaders,    A11yCheckId::HeadingHierarchy,
            A11yCheckId::ReadingOrder,    A11yCheckId::Contrast,
        };
        QCOMPARE(report.coverage.size(), allChecks.size());
        for (const A11yCheckId id : allChecks) {
            const bool found = std::any_of(report.coverage.begin(), report.coverage.end(),
                                           [id](const A11yCoverageEntry& e) { return e.check == id; });
            QVERIFY2(found, describe(id));
        }
        QVERIFY(!report.notCoveredAtAll.empty());
    }

    void untaggedDocumentOnlyReportsDocumentLevelDefects() {
        const QByteArray pdf = alioth::test::makeUntaggedPdf();
        PdfSourceDocument source;
        QCOMPARE(static_cast<int>(source.open(alioth::test::toStdString(pdf))),
                 static_cast<int>(SourceStatus::Ok));
        const StructTree tree = readStructTree(source);
        QCOMPARE(static_cast<int>(tree.status), static_cast<int>(StructTreeStatus::NoStructTree));

        const A11yReport report = runAccessibilityCheck(source, tree);
        QVERIFY(hasFinding(report, A11yCheckId::TagStructure));
        // 沒有結構樹時，依賴結構樹的檢查不該硬跑出一堆假發現。
        QVERIFY(!hasFinding(report, A11yCheckId::ImageAltText));
        QVERIFY(!hasFinding(report, A11yCheckId::TableHeaders));
        QVERIFY(!hasFinding(report, A11yCheckId::HeadingHierarchy));
        QVERIFY(!hasFinding(report, A11yCheckId::ReadingOrder));
    }

    void cleanTaggedDocumentHasNoAltTextFinding() {
        // 沿用既有的標籤化語料：其中一個 Figure 有 /Alt，用來確認「有替代文字」
        // 不會被誤判成缺失——只驗證「抓得到問題」不夠，也要驗證「不會抓錯」。
        const QByteArray pdf = alioth::test::makeTaggedPdf();
        PdfSourceDocument source;
        QCOMPARE(static_cast<int>(source.open(alioth::test::toStdString(pdf))),
                 static_cast<int>(SourceStatus::Ok));
        const StructTree tree = readStructTree(source);
        const A11yReport report = runAccessibilityCheck(source, tree);

        int figureFindings = 0;
        for (const A11yFinding& finding : report.findings) {
            if (finding.check == A11yCheckId::ImageAltText) ++figureFindings;
        }
        // makeTaggedPdf 有兩個 Figure，一個有 /Alt 一個沒有：應該只抓到一個。
        QCOMPARE(figureFindings, 1);
    }
};

QTEST_APPLESS_MAIN(TestAccessibilityChecker)
#include "test_accessibility_checker.moc"
