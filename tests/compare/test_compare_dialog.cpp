// 文件比較結果的呈現（PRD-CMP-001）。
//
// 差異怎麼算由 engine/compare 的測試守著。這裡驗的是呈現上的三個判斷，
// 而它們都關係到「使用者會不會誤判自己看到了什麼」：
//
//   - 沒有差異必須說出來，不能只留一張空清單
//   - 降級的比對必須標明，否則使用者以為看到的是最精細的結果
//   - 沒變的頁面不列出來，否則真正要看的那幾頁被埋在幾百列裡

#include <QtTest>

#include <QLabel>
#include <QListWidget>
#include <QTreeWidget>

#include "ui/compare_dialog.h"

using alioth::domain::DiffKind;
using alioth::domain::DocumentDiff;
using alioth::domain::PageDiffSummary;
using alioth::domain::PageMatchKind;
using alioth::domain::TextDiffRegion;
using alioth::ui::CompareDialog;

namespace {

DocumentDiff identicalDiff() {
    DocumentDiff diff;
    std::vector<PageDiffSummary> summaries;
    for (int i = 0; i < 3; ++i) {
        PageDiffSummary summary;
        summary.kind = PageMatchKind::Matched;
        summary.oldPage = i;
        summary.newPage = i;
        summary.similarity = 1.0;
        summaries.push_back(summary);
    }
    diff.setSummaries(std::move(summaries));
    return diff;
}

DocumentDiff diffWithOneChangedPage() {
    DocumentDiff diff;
    std::vector<PageDiffSummary> summaries;

    PageDiffSummary unchanged;
    unchanged.kind = PageMatchKind::Matched;
    unchanged.oldPage = 0;
    unchanged.newPage = 0;
    unchanged.similarity = 1.0;
    summaries.push_back(unchanged);

    PageDiffSummary changed;
    changed.kind = PageMatchKind::Matched;
    changed.oldPage = 1;
    changed.newPage = 1;
    changed.similarity = 0.6;
    changed.replacements = 1;
    summaries.push_back(changed);

    diff.setSummaries(std::move(summaries));

    TextDiffRegion region;
    region.kind = DiffKind::Replace;
    region.oldPage = 1;
    region.newPage = 1;
    region.oldText = "before";
    region.newText = "after";
    diff.addRegion(std::move(region));
    return diff;
}

}  // namespace

class TestCompareDialog : public QObject {
    Q_OBJECT

private slots:
    void identicalDocumentsSaySoInsteadOfShowingAnEmptyList();
    void onlyChangedPagesAreListed();
    void degradedComparisonIsAnnounced();
    void selectingAPageShowsItsRegions();
};

void TestCompareDialog::identicalDocumentsSaySoInsteadOfShowingAnEmptyList() {
    const DocumentDiff diff = identicalDiff();
    CompareDialog dialog(diff, QStringLiteral("a.pdf"), QStringLiteral("b.pdf"));

    auto* summary = dialog.findChild<QLabel*>(QStringLiteral("compareSummary"));
    auto* pages = dialog.findChild<QTreeWidget*>(QStringLiteral("comparePages"));
    QVERIFY(summary != nullptr && pages != nullptr);

    // 空清單無法分辨「真的一樣」與「比對失敗」。
    QCOMPARE(pages->topLevelItemCount(), 0);
    QVERIFY2(!summary->text().isEmpty(), "沒有差異時什麼都沒說");
    QVERIFY(summary->text().contains(QStringLiteral("相同")));
}

void TestCompareDialog::onlyChangedPagesAreListed() {
    const DocumentDiff diff = diffWithOneChangedPage();
    CompareDialog dialog(diff, QStringLiteral("a.pdf"), QStringLiteral("b.pdf"));

    auto* pages = dialog.findChild<QTreeWidget*>(QStringLiteral("comparePages"));
    QVERIFY(pages != nullptr);
    // 兩頁裡只有一頁變了。把沒變的也列出來，真正要看的那一頁會被埋掉。
    QCOMPARE(pages->topLevelItemCount(), 1);
    // 頁碼顯示成 1 起算。
    QCOMPARE(pages->topLevelItem(0)->text(2), QStringLiteral("2"));
}

void TestCompareDialog::degradedComparisonIsAnnounced() {
    DocumentDiff diff = diffWithOneChangedPage();
    diff.setDegraded(true);
    CompareDialog dialog(diff, QStringLiteral("a.pdf"), QStringLiteral("b.pdf"));

    auto* summary = dialog.findChild<QLabel*>(QStringLiteral("compareSummary"));
    QVERIFY(summary != nullptr);
    // 降級的結果仍然正確，但不是最小差異。不說的話使用者會以為看到的
    // 就是最精細的比對。
    QVERIFY2(summary->text().contains(QStringLiteral("整段替換")),
             qPrintable(summary->text()));
}

void TestCompareDialog::selectingAPageShowsItsRegions() {
    const DocumentDiff diff = diffWithOneChangedPage();
    CompareDialog dialog(diff, QStringLiteral("a.pdf"), QStringLiteral("b.pdf"));

    auto* regions = dialog.findChild<QListWidget*>(QStringLiteral("compareRegions"));
    QVERIFY(regions != nullptr);
    // 建構時就選了第一列，所以差異段落應該已經填好。
    QCOMPARE(regions->count(), 1);
    QVERIFY(regions->item(0)->text().contains(QStringLiteral("before")));
    QVERIFY(regions->item(0)->text().contains(QStringLiteral("after")));
}

QTEST_MAIN(TestCompareDialog)
#include "test_compare_dialog.moc"
