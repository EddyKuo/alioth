// 匯出選定註解的比對規則（PRD-ANN-013）。
//
// 畫面上的註解清單與匯出的內容來自**兩條不同的讀取路徑**：清單來自 PDFium
// 的列舉，匯出來自物件層的 /Annots 走訪。兩者對 Popup 之類的附屬註解是否
// 計入並不保證一致，所以「使用者選了第 3 列」不能翻譯成「匯出第 3 個 entry」
// ——一旦錯開，使用者選 A 卻匯出 B，而輸出檔看起來完全正常。
//
// 因此比對用的是內容本身（頁碼 + 子型 + 外框 + 作者 + 內容）。這支測試守的
// 就是那條規則，以及它的兩個邊界：完全相同的兩則不可以被同一個選取項配對
// 兩次，對不上的選取項不可以憑空補一則出來。

#include <QtTest>

#include "app/annotation_service.h"
#include "app/xfdf_io.h"
#include "domain/annotation.h"

using namespace alioth;
using alioth::app::AnnotationService;
using alioth::app::XfdfEntry;

namespace {

XfdfEntry makeEntry(int page, const domain::RectF& rect, const std::string& author,
                    const std::string& contents) {
    XfdfEntry entry;
    entry.pageIndex = page;
    entry.annotation.rect = rect;
    entry.annotation.author = author;
    entry.annotation.contents = contents;
    entry.annotation.geometry = domain::ShapeGeometry{};
    return entry;
}

domain::AnnotationSummary summaryOf(const XfdfEntry& entry, int indexOnPage = 0) {
    domain::AnnotationSummary summary;
    summary.pageIndex = entry.pageIndex;
    summary.indexOnPage = indexOnPage;
    summary.subtype = domain::subtypeName(entry.annotation.type());
    summary.author = entry.annotation.author;
    summary.contents = entry.annotation.contents;
    summary.rect = entry.annotation.rect;
    return summary;
}

}  // namespace

class TestAnnotationSelection : public QObject {
    Q_OBJECT

private slots:
    void selectsOnlyTheRequestedEntries() {
        const XfdfEntry first = makeEntry(0, {10, 10, 50, 30}, "Alice", "first");
        const XfdfEntry second = makeEntry(1, {20, 20, 60, 40}, "Bob", "second");
        const XfdfEntry third = makeEntry(1, {80, 20, 120, 40}, "Bob", "third");

        const auto selected =
            AnnotationService::selectEntries({first, second, third}, {summaryOf(third)});
        QCOMPARE(static_cast<int>(selected.size()), 1);
        QCOMPARE(selected[0].annotation.contents, std::string("third"));
    }

    void emptySelectionExportsNothing() {
        const XfdfEntry only = makeEntry(0, {10, 10, 50, 30}, "Alice", "only");
        QVERIFY(AnnotationService::selectEntries({only}, {}).empty());
    }

    // 頁碼不同就不是同一則，即使其餘欄位完全相同。範本化的審閱意見
    // （「請補充說明」）在同一份文件裡出現十次是常態。
    void sameContentOnAnotherPageIsNotAMatch() {
        const XfdfEntry onPageZero = makeEntry(0, {10, 10, 50, 30}, "Alice", "same");
        const XfdfEntry onPageFive = makeEntry(5, {10, 10, 50, 30}, "Alice", "same");

        const auto selected =
            AnnotationService::selectEntries({onPageZero, onPageFive}, {summaryOf(onPageFive)});
        QCOMPARE(static_cast<int>(selected.size()), 1);
        QCOMPARE(selected[0].pageIndex, 5);
    }

    // 兩則完全相同的註解是合法的（同一個位置蓋兩次）。選了一則就只能匯出
    // 一則——配對過的 entry 不再參與比對。
    void identicalAnnotationsAreMatchedOneForOne() {
        const XfdfEntry a = makeEntry(0, {10, 10, 50, 30}, "Alice", "dup");
        const XfdfEntry b = a;

        QCOMPARE(static_cast<int>(AnnotationService::selectEntries({a, b}, {summaryOf(a)}).size()),
                 1);
        QCOMPARE(static_cast<int>(
                     AnnotationService::selectEntries({a, b}, {summaryOf(a), summaryOf(b)}).size()),
                 2);
    }

    // 對不上的選取項目不會憑空補一則出來。檔案在這期間被別的程式改過時
    // 就是這個情形，呼叫端必須據此告訴使用者，而不是交出一份少了幾則的檔案。
    void unmatchedSelectionYieldsFewerEntries() {
        const XfdfEntry present = makeEntry(0, {10, 10, 50, 30}, "Alice", "present");
        domain::AnnotationSummary missing = summaryOf(present);
        missing.contents = "edited elsewhere";

        const auto selected =
            AnnotationService::selectEntries({present}, {summaryOf(present), missing});
        QCOMPARE(static_cast<int>(selected.size()), 1);
    }

    // 外框用容差比對：兩條路徑各自把座標轉過一輪浮點運算，逐位元組相等
    // 是過強的要求。但容差要小到不可能把相鄰的兩則認成同一則。
    void tinyCoordinateDifferencesStillMatch() {
        const XfdfEntry entry = makeEntry(0, {10.0, 10.0, 50.0, 30.0}, "Alice", "x");
        domain::AnnotationSummary nudged = summaryOf(entry);
        nudged.rect = domain::RectF{10.05, 9.98, 50.02, 30.01};
        QCOMPARE(static_cast<int>(AnnotationService::selectEntries({entry}, {nudged}).size()), 1);

        domain::AnnotationSummary moved = summaryOf(entry);
        moved.rect = domain::RectF{30.0, 10.0, 70.0, 30.0};
        QVERIFY(AnnotationService::selectEntries({entry}, {moved}).empty());
    }
};

QTEST_MAIN(TestAnnotationSelection)
#include "test_annotation_selection.moc"
