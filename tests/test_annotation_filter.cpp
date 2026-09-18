// 註解列表的篩選與排序（PRD-ANN-008）。
//
// 這組測試釘的多半是「排序看起來會自己跳動」與「跨時區排錯」這兩類——
// 兩者都不會崩潰，只會讓使用者不再信任這個列表。

#include <QtTest>

#include "domain/annotation_filter.h"

using namespace alioth::domain;

namespace {

AnnotationSummary make(std::int32_t page, std::int32_t indexOnPage, const char* subtype,
                       const char* author, const char* modified, const char* contents = "") {
    AnnotationSummary summary;
    summary.pageIndex = page;
    summary.indexOnPage = indexOnPage;
    summary.subtype = subtype;
    summary.author = author;
    summary.modified = modified;
    summary.contents = contents;
    return summary;
}

std::vector<AnnotationSummary> corpus() {
    return {
        make(2, 0, "Highlight", "Bob", "D:20260901120000+08'00'", "check this"),
        make(0, 1, "Square", "Alice", "D:20260903090000+08'00'", "layout"),
        make(0, 0, "Highlight", "Alice", "D:20260902080000+08'00'", "typo"),
        make(1, 0, "Ink", "Bob", "D:20260902083000+08'00'", ""),
    };
}

}  // namespace

class TestAnnotationFilter : public QObject {
    Q_OBJECT

private slots:
    void emptyFilterKeepsEverythingInDocumentOrder() {
        const auto list = corpus();
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Page, true);
        QCOMPARE(order.size(), std::size_t(4));
        // 文件順序是頁碼再頁內序號，不是輸入順序。
        QCOMPARE(list[order[0]].pageIndex, 0);
        QCOMPARE(list[order[0]].indexOnPage, 0);
        QCOMPARE(list[order[1]].indexOnPage, 1);
        QCOMPARE(list[order[2]].pageIndex, 1);
        QCOMPARE(list[order[3]].pageIndex, 2);
    }

    void filterByPage() {
        const auto list = corpus();
        AnnotationFilter filter;
        filter.pageIndex = 0;
        const auto order = filterAndSort(list, filter, AnnotationSortKey::Page, true);
        QCOMPARE(order.size(), std::size_t(2));
        for (const std::size_t index : order) QCOMPARE(list[index].pageIndex, 0);
    }

    void filterByAuthorAndType() {
        const auto list = corpus();
        AnnotationFilter filter;
        filter.author = "Alice";
        filter.subtype = "Highlight";
        const auto order = filterAndSort(list, filter, AnnotationSortKey::Page, true);
        QCOMPARE(order.size(), std::size_t(1));
        QCOMPARE(list[order[0]].contents, std::string("typo"));
    }

    void textSearchIsCaseInsensitiveAndCoversAuthor() {
        const auto list = corpus();
        AnnotationFilter byContents;
        byContents.text = "CHECK";
        QCOMPARE(filterAndSort(list, byContents, AnnotationSortKey::Page, true).size(),
                 std::size_t(1));

        AnnotationFilter byAuthor;
        byAuthor.text = "alice";
        QCOMPARE(filterAndSort(list, byAuthor, AnnotationSortKey::Page, true).size(),
                 std::size_t(2));
    }

    void sortByDateUsesRealTimeNotStringOrder() {
        const auto list = corpus();
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Date, true);
        QCOMPARE(list[order[0]].modified, std::string("D:20260901120000+08'00'"));
        QCOMPARE(list[order[3]].modified, std::string("D:20260903090000+08'00'"));
    }

    void dateComparisonCrossesTimeZonesCorrectly() {
        // 同一個瞬間，兩種時區寫法。字串比對會判定它們不同且順序錯誤。
        // 09:00+08:00 與 01:00+00:00 是同一時刻。
        QCOMPARE(parsePdfDate("D:20260906090000+08'00'"),
                 parsePdfDate("D:20260906010000+00'00'"));
        // 東八區的 08:00 早於 UTC 的 08:00。
        QVERIFY(parsePdfDate("D:20260906080000+08'00'") <
                parsePdfDate("D:20260906080000+00'00'"));
    }

    void malformedDatesSortTogetherInsteadOfScattering() {
        std::vector<AnnotationSummary> list = corpus();
        list.push_back(make(3, 0, "Text", "Carol", "not a date"));
        list.push_back(make(4, 0, "Text", "Carol", ""));
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Date, true);
        // 解析不出來的一律是 0，因此集中在最前面，而不是散在中間。
        QCOMPARE(parsePdfDate(list[order[0]].modified), std::int64_t(0));
        QCOMPARE(parsePdfDate(list[order[1]].modified), std::int64_t(0));
        QVERIFY(parsePdfDate(list[order[2]].modified) > 0);
    }

    void optionalFieldsGetSpecDefaults() {
        // 只寫到年月日的產生器不少。缺席欄位取規格預設值（月日 01、時分秒 00），
        // 而不是視為無法解析——那會讓一整批註解被歸到「沒有日期」。
        QVERIFY(parsePdfDate("D:2026") > 0);
        QCOMPARE(parsePdfDate("D:20260101000000"), parsePdfDate("D:2026"));
        // 沒有 D: 前綴也要能讀。
        QCOMPARE(parsePdfDate("20260101000000"), parsePdfDate("D:20260101000000"));
    }

    void sortIsStableWithinEqualKeys() {
        // 依作者排序時，同一位作者的註解必須維持文件順序。少了這條，
        // 每次重新排序的結果都可能不同，使用者會覺得列表自己在跳。
        const auto list = corpus();
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Author, true);
        // Alice 的兩則：第 0 頁第 0 個要排在第 0 頁第 1 個之前。
        QCOMPARE(list[order[0]].author, std::string("Alice"));
        QCOMPARE(list[order[0]].indexOnPage, 0);
        QCOMPARE(list[order[1]].author, std::string("Alice"));
        QCOMPARE(list[order[1]].indexOnPage, 1);
    }

    void descendingReversesPrimaryKeyOnly() {
        const auto list = corpus();
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Author, false);
        QCOMPARE(list[order[0]].author, std::string("Bob"));
        // 次要鍵仍然是文件順序，不跟著反轉——反轉次要鍵會讓「由大到小」
        // 變成兩個維度同時翻面，那不是使用者按下遞減時期待的事。
        QCOMPARE(list[order[0]].pageIndex, 1);
        QCOMPARE(list[order[1]].pageIndex, 2);
    }

    void distinctListsFeedTheFilterDropdowns() {
        const auto list = corpus();
        const auto authors = distinctAuthors(list);
        QCOMPARE(authors.size(), std::size_t(2));
        QCOMPARE(authors[0], std::string("Alice"));
        const auto subtypes = distinctSubtypes(list);
        QCOMPARE(subtypes.size(), std::size_t(3));
    }

    void filteringDoesNotLoseTheMappingBackToTheOriginalList() {
        // 回傳索引而不是複本，正是為了讓面板點選第 N 列時跳到正確的註解。
        const auto list = corpus();
        AnnotationFilter filter;
        filter.author = "Bob";
        const auto order = filterAndSort(list, filter, AnnotationSortKey::Page, true);
        QCOMPARE(order.size(), std::size_t(2));
        for (const std::size_t index : order) {
            QVERIFY(index < list.size());
            QCOMPARE(list[index].author, std::string("Bob"));
        }
    }

    // 上一則／下一則註解（PDF-XChange 的 Previous / Next Comment）。
    //
    // 走文件順序而不是清單目前的排序：清單可以依作者或日期排，但「下一則」
    // 在使用者心裡是「往文件後面走」。語料的輸入順序刻意與文件順序不同，
    // 所以拿輸入順序當答案的實作在這裡會紅。
    void adjacentWalksTheDocumentOrderNotTheInputOrder() {
        const auto list = corpus();
        // 文件順序：(0,0) typo → (0,1) layout → (1,0) ink → (2,0) check this
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Page, true);

        std::size_t current = order[0];
        for (std::size_t step = 1; step < order.size(); ++step) {
            current = adjacentInDocumentOrder(list, current, 1);
            QCOMPARE(current, order[step]);
        }
        // 往回也要走回同一條路。
        for (std::size_t step = order.size() - 1; step > 0; --step) {
            current = adjacentInDocumentOrder(list, current, -1);
            QCOMPARE(current, order[step - 1]);
        }
    }

    // 還沒有選取時：往後從第一則開始，往前從最後一則開始。
    void adjacentStartsFromTheEndsWhenNothingIsSelected() {
        const auto list = corpus();
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Page, true);
        const std::size_t none = list.size();

        QCOMPARE(adjacentInDocumentOrder(list, none, 1), order.front());
        QCOMPARE(adjacentInDocumentOrder(list, none, -1), order.back());
    }

    // 到頭與到尾不繞回去。繞回去的話，使用者連按到底會不知不覺回到第一則，
    // 以為自己漏看了中間幾則又走一遍。
    void adjacentDoesNotWrapAround() {
        const auto list = corpus();
        const auto order = filterAndSort(list, AnnotationFilter{}, AnnotationSortKey::Page, true);

        QCOMPARE(adjacentInDocumentOrder(list, order.back(), 1), list.size());
        QCOMPARE(adjacentInDocumentOrder(list, order.front(), -1), list.size());
    }

    void adjacentOnAnEmptyListIsHarmless() {
        const std::vector<AnnotationSummary> empty;
        QCOMPARE(adjacentInDocumentOrder(empty, 0, 1), std::size_t(0));
        QCOMPARE(adjacentInDocumentOrder(empty, 0, -1), std::size_t(0));
    }
};

QTEST_APPLESS_MAIN(TestAnnotationFilter)
#include "test_annotation_filter.moc"
