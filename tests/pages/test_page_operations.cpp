// 頁面操作領域層測試（WBS 5.6–5.8 的純邏輯部分）。
//
// 這裡一個 PDFium 呼叫都沒有：頁面範圍怎麼解讀、多頁同時異動之後順序長什麼樣、
// 分割規則切在哪裡，全部是純邏輯。把它們獨立測掉之後，引擎層的測試失敗就只會有
// 一個原因——PDFium 的行為與我們的理解不符。

#include <QtTest>

#include <string>
#include <vector>

#include "domain/page_operations.h"

using namespace alioth::domain::pages;

namespace {

std::vector<int> parsed(const std::string& text, int pageCount, const PageRangeOptions& options = {}) {
    return parsePageRange(text, pageCount, options).pages;
}

std::vector<int> sourceIndices(const PageOrder& order) {
    std::vector<int> out;
    out.reserve(order.size());
    for (const PageSlot& slot : order) out.push_back(slot.sourceIndex);
    return out;
}

}  // namespace

class TestPageOperations : public QObject {
    Q_OBJECT

private slots:
    // ---- 頁面範圍解析 -----------------------------------------------------

    void parsesSinglePagesAndRanges() {
        QCOMPARE(parsed("1-3,7", 10), (std::vector<int>{0, 1, 2, 6}));
        QCOMPARE(parsed("5", 10), (std::vector<int>{4}));
        // 空白在任何位置都不影響語意。
        QCOMPARE(parsed("  1 - 3 ,\t7 ", 10), (std::vector<int>{0, 1, 2, 6}));
        // 分號與逗號等價，貼上來的清單常常混用。
        QCOMPARE(parsed("1;3", 10), (std::vector<int>{0, 2}));
    }

    void parsesOpenEndedRanges() {
        QCOMPARE(parsed("9-", 10), (std::vector<int>{8, 9}));
        QCOMPARE(parsed("-3", 10), (std::vector<int>{0, 1, 2}));
        QCOMPARE(parsed("1-", 3), (std::vector<int>{0, 1, 2}));
    }

    void rejectsEmptyInput() {
        QCOMPARE(parsePageRange("", 10).status, RangeParseStatus::EmptyInput);
        QCOMPARE(parsePageRange("   ", 10).status, RangeParseStatus::EmptyInput);
        // 空區段是錯字，不能當成「忽略就好」。
        QCOMPARE(parsePageRange("1,,3", 10).status, RangeParseStatus::SyntaxError);
        QCOMPARE(parsePageRange("1,", 10).status, RangeParseStatus::SyntaxError);
    }

    void rejectsSyntaxErrorsWithOffset() {
        const PageRangeParse bad = parsePageRange("1-3,abc", 10);
        QCOMPARE(bad.status, RangeParseStatus::SyntaxError);
        QCOMPARE(bad.errorOffset, std::size_t{4});
        QCOMPARE(parsePageRange("1-2-3", 10).status, RangeParseStatus::SyntaxError);
        // 單獨一個減號不是「全部」：全選在 UI 上另有入口，讓它有兩種寫法只會吃掉錯字。
        QCOMPARE(parsePageRange("-", 10).status, RangeParseStatus::SyntaxError);
        QCOMPARE(parsePageRange("+3", 10).status, RangeParseStatus::SyntaxError);
    }

    void rejectsZeroPage() {
        QCOMPARE(parsePageRange("0", 10).status, RangeParseStatus::ZeroOrNegative);
        QCOMPARE(parsePageRange("0-3", 10).status, RangeParseStatus::ZeroOrNegative);
    }

    void rejectsInvalidPageCount() {
        QCOMPARE(parsePageRange("1", 0).status, RangeParseStatus::InvalidPageCount);
    }

    void outOfRangeIsRejectedByDefault() {
        QCOMPARE(parsePageRange("7", 5).status, RangeParseStatus::OutOfRange);
        QCOMPARE(parsePageRange("3-99", 5).status, RangeParseStatus::OutOfRange);
    }

    void outOfRangeCanBeClamped() {
        PageRangeOptions options;
        options.outOfRange = OutOfRangePolicy::Clamp;
        // 區間夾端點。
        QCOMPARE(parsed("3-99", 5, options), (std::vector<int>{2, 3, 4}));
        // 完全落在文件外的區間整段丟掉，其他區段照常。
        QCOMPARE(parsed("1,9-12", 5, options), (std::vector<int>{0}));
        // 單頁越界是丟棄而不是夾到最後一頁：夾了就會安靜地擷取出使用者沒要的內容。
        QCOMPARE(parsed("2,7", 5, options), (std::vector<int>{1}));
        // 全部被丟光時要有專屬狀態，而不是「成功但沒東西」。
        QCOMPARE(parsePageRange("9-12", 5, options).status, RangeParseStatus::EmptyResult);
    }

    void overlappingRangesMergeByDefault() {
        QCOMPARE(parsed("1-3,2-5", 10), (std::vector<int>{0, 1, 2, 3, 4}));
        // 去重保留第一次出現的位置，不重新排序。
        QCOMPARE(parsed("5,1-3,5", 10), (std::vector<int>{4, 0, 1, 2}));
    }

    void overlappingRangesCanBeKept() {
        PageRangeOptions options;
        options.duplicates = DuplicatePolicy::Keep;
        QCOMPARE(parsed("1,1,2", 10, options), (std::vector<int>{0, 0, 1}));
    }

    void descendingRangeExpandsByDefault() {
        QCOMPARE(parsed("5-3", 10), (std::vector<int>{4, 3, 2}));
        QCOMPARE(parsed("3-1,5", 10), (std::vector<int>{2, 1, 0, 4}));
    }

    void descendingRangeCanBeRejected() {
        PageRangeOptions options;
        options.descending = DescendingPolicy::Reject;
        QCOMPARE(parsePageRange("5-3", 10, options).status, RangeParseStatus::DescendingRange);
    }

    void formatsCanonicalRange() {
        QCOMPARE(formatPageRange({0, 1, 2, 6}), std::string("1-3,7"));
        QCOMPARE(formatPageRange({4}), std::string("5"));
        // 遞減不縮成區間，往返轉換才不依賴解析政策。
        QCOMPARE(formatPageRange({4, 3, 2}), std::string("5,4,3"));
        QCOMPARE(formatPageRange({}), std::string(""));
    }

    void filtersByParity() {
        const std::vector<int> all = allPages(6);
        QCOMPARE(filterByParity(all, PageParity::Odd), (std::vector<int>{0, 2, 4}));
        QCOMPARE(filterByParity(all, PageParity::Even), (std::vector<int>{1, 3, 5}));
        QCOMPARE(filterByParity(all, PageParity::All), all);
    }

    // ---- 旋轉 -------------------------------------------------------------

    void rotationWrapsAround() {
        QCOMPARE(rotationFromQuarterTurns(4), PageRotation::None);
        QCOMPARE(rotationFromQuarterTurns(-1), PageRotation::CounterClockwise90);
        QCOMPARE(combine(PageRotation::Clockwise90, PageRotation::CounterClockwise90),
                 PageRotation::None);
        QCOMPARE(combine(PageRotation::Half, PageRotation::Half), PageRotation::None);
        QCOMPARE(degreesOf(PageRotation::CounterClockwise90), 270);
    }

    void rotationFromDegreesRejectsNonMultiples() {
        PageRotation rotation = PageRotation::None;
        QVERIFY(rotationFromDegrees(180, rotation));
        QCOMPARE(rotation, PageRotation::Half);
        QVERIFY(rotationFromDegrees(-90, rotation));
        QCOMPARE(rotation, PageRotation::CounterClockwise90);
        // 45 度不四捨五入成 90：悄悄取整會讓呼叫端以為設定成功了。
        QVERIFY(!rotationFromDegrees(45, rotation));
    }

    // ---- 操作模擬 ---------------------------------------------------------

    void insertsBlankPages() {
        const SimulationResult result =
            applyOperation(identityOrder(3), InsertBlankPages{1, 2, 100.0, 200.0});
        QVERIFY(result.ok());
        QCOMPARE(sourceIndices(result.order), (std::vector<int>{0, kNewPage, kNewPage, 1, 2}));
    }

    void insertRejectsBadArguments() {
        QCOMPARE(validate(InsertBlankPages{0, 0, 100.0, 100.0}, 3), OperationStatus::InvalidCount);
        QCOMPARE(validate(InsertBlankPages{0, 1, 0.0, 100.0}, 3), OperationStatus::InvalidGeometry);
        QCOMPARE(validate(InsertBlankPages{4, 1, 100.0, 100.0}, 3),
                 OperationStatus::InvalidDestination);
        // 插在最後一頁之後是合法的（等於附加）。
        QCOMPARE(validate(InsertBlankPages{3, 1, 100.0, 100.0}, 3), OperationStatus::Ok);
    }

    void deleteKeepsRemainingOrder() {
        const SimulationResult result = applyOperation(identityOrder(5), DeletePages{{1, 3}});
        QVERIFY(result.ok());
        QCOMPARE(sourceIndices(result.order), (std::vector<int>{0, 2, 4}));
    }

    void deleteRejectsDuplicatesAndEmptyResult() {
        QCOMPARE(validate(DeletePages{{}}, 5), OperationStatus::EmptySelection);
        QCOMPARE(validate(DeletePages{{1, 1}}, 5), OperationStatus::DuplicateSelection);
        QCOMPARE(validate(DeletePages{{5}}, 5), OperationStatus::IndexOutOfRange);
        QCOMPARE(validate(DeletePages{{0, 1, 2}}, 3), OperationStatus::WouldEmptyDocument);
    }

    void rotateAccumulatesWhenRelative() {
        PageOrder order = identityOrder(2);
        order = applyOperation(order, RotatePages{{0}, PageRotation::Clockwise90, true}).order;
        order = applyOperation(order, RotatePages{{0}, PageRotation::Clockwise90, true}).order;
        QCOMPARE(order[0].rotation, PageRotation::Half);
        QCOMPARE(order[1].rotation, PageRotation::None);

        order = applyOperation(order, RotatePages{{0}, PageRotation::Clockwise90, false}).order;
        QCOMPARE(order[0].rotation, PageRotation::Clockwise90);
    }

    void moveKeepsSelectionOrder() {
        // [A B C D] 把 D、C 移到索引 1 → [A D C B]（與 PDFium 標頭的例子相同）。
        const SimulationResult result = applyOperation(identityOrder(4), MovePages{{3, 2}, 1});
        QVERIFY(result.ok());
        QCOMPARE(sourceIndices(result.order), (std::vector<int>{0, 3, 2, 1}));
    }

    void moveRejectsInvalidDestination() {
        QCOMPARE(validate(MovePages{{0, 3, 1}, 3}, 4), OperationStatus::InvalidDestination);
        QCOMPARE(validate(MovePages{{2, 2}, 0}, 4), OperationStatus::DuplicateSelection);
        QCOMPARE(validate(MovePages{{0, 4}, 1}, 4), OperationStatus::IndexOutOfRange);
    }

    void duplicateAllowsRepeatedSelection() {
        const SimulationResult result = applyOperation(identityOrder(3), DuplicatePages{{0, 0}, 3});
        QVERIFY(result.ok());
        QCOMPARE(sourceIndices(result.order), (std::vector<int>{0, 1, 2, 0, 0}));
    }

    void swapExchangesTwoPages() {
        const SimulationResult result = applyOperation(identityOrder(4), SwapPages{1, 3});
        QVERIFY(result.ok());
        QCOMPARE(sourceIndices(result.order), (std::vector<int>{0, 3, 2, 1}));
        QCOMPARE(validate(SwapPages{0, 4}, 4), OperationStatus::IndexOutOfRange);
    }

    void reverseFlipsWholeDocument() {
        const SimulationResult result = applyOperation(identityOrder(4), ReversePages{});
        QVERIFY(result.ok());
        QCOMPARE(sourceIndices(result.order), (std::vector<int>{3, 2, 1, 0}));
    }

    // ---- 分割規則 ---------------------------------------------------------

    void splitsEveryNPages() {
        const SplitPlan plan = planSplit(SplitRule{SplitMode::EveryNPages, 2, 0, {}}, 5);
        QVERIFY(plan.ok());
        QCOMPARE(plan.chunks, (std::vector<SplitChunk>{{0, 1}, {2, 3}, {4, 4}}));
        QCOMPARE(planSplit(SplitRule{SplitMode::EveryNPages, 0, 0, {}}, 5).status,
                 SplitStatus::InvalidChunkSize);
    }

    void splitsByMaxBytes() {
        SplitRule rule{SplitMode::MaxBytes, 1, 100, {}};
        const SplitPlan plan = planSplit(rule, 4, {40, 50, 30, 60});
        QVERIFY(plan.ok());
        // 40+50=90 放得下，再加 30 會超過 100 → 切開。
        QCOMPARE(plan.chunks, (std::vector<SplitChunk>{{0, 1}, {2, 3}}));
        QVERIFY(!plan.hasOversizedChunk);

        // 單頁就超過上限時，它自己成為一檔並超標；這件事必須被回報。
        const SplitPlan oversized = planSplit(rule, 3, {40, 500, 30});
        QVERIFY(oversized.ok());
        QVERIFY(oversized.hasOversizedChunk);
        QCOMPARE(oversized.chunks, (std::vector<SplitChunk>{{0, 0}, {1, 1}, {2, 2}}));

        QCOMPARE(planSplit(rule, 4, {}).status, SplitStatus::MissingPageSizes);
        QCOMPARE(planSplit(SplitRule{SplitMode::MaxBytes, 1, 0, {}}, 4, {1, 1, 1, 1}).status,
                 SplitStatus::InvalidBudget);
    }

    void splitsAtGivenPages() {
        // 邊界未排序且有重複，結果必須一樣。
        const SplitPlan plan = planSplit(SplitRule{SplitMode::AtPageNumbers, 1, 0, {4, 2, 2}}, 6);
        QVERIFY(plan.ok());
        QCOMPARE(plan.chunks, (std::vector<SplitChunk>{{0, 1}, {2, 3}, {4, 5}}));
        // 在第一頁之前切等於沒切。
        QCOMPARE(planSplit(SplitRule{SplitMode::AtPageNumbers, 1, 0, {0}}, 3).chunks,
                 (std::vector<SplitChunk>{{0, 2}}));
        QCOMPARE(planSplit(SplitRule{SplitMode::AtPageNumbers, 1, 0, {9}}, 3).status,
                 SplitStatus::InvalidBoundary);
    }

    void splitRejectsEmptyDocument() {
        QCOMPARE(planSplit(SplitRule{}, 0).status, SplitStatus::InvalidPageCount);
    }
};

QTEST_APPLESS_MAIN(TestPageOperations)
#include "test_page_operations.moc"
