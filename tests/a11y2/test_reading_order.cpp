// 閱讀順序比對測試（PRD-A11Y-002）。
//
// 刻意直接建構 StructNode／StructTree 值，不經過真正的 PDF 剖析：
// computePageReadingOrder 是純函數，輸入只在意 pageIndex／mcids／children，
// 用假 PDF 語料測反而會讓測試多背一份與本測試無關的檔案格式細節。

#include <QtTest>

#include <algorithm>

#include "engine/objects/reading_order.h"

using namespace alioth::engine::objects;

namespace {

StructNode makeLeaf(std::string type, std::int32_t page, std::vector<std::int32_t> mcids) {
    StructNode node;
    node.type = std::move(type);
    node.pageIndex = page;
    node.mcids = std::move(mcids);
    return node;
}

}  // namespace

class TestReadingOrder : public QObject {
    Q_OBJECT

private slots:
    void consistentOrderHasNoMismatch() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(makeLeaf("P", 0, {0}));
        doc.children.push_back(makeLeaf("P", 0, {1}));
        doc.children.push_back(makeLeaf("P", 0, {2}));
        tree.roots.push_back(std::move(doc));

        const PageReadingOrder order = computePageReadingOrder(tree, 0);
        QCOMPARE(order.items.size(), std::size_t{3});
        QVERIFY(order.mismatchIndices.empty());
        QCOMPARE(order.unknownContentOrderCount, 0);
        for (std::size_t i = 0; i < order.items.size(); ++i) {
            QCOMPARE(order.items[i].structureRank, static_cast<int>(i));
            QCOMPARE(order.items[i].contentRank, static_cast<int>(i));
        }
    }

    void reorderedStructureFlagsMismatch() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        // 結構順序是 5, 1, 9——但內容其實是照 1 之後才畫 5，兩者在頭兩項不一致。
        doc.children.push_back(makeLeaf("P", 0, {5}));
        doc.children.push_back(makeLeaf("P", 0, {1}));
        doc.children.push_back(makeLeaf("P", 0, {9}));
        tree.roots.push_back(std::move(doc));

        const PageReadingOrder order = computePageReadingOrder(tree, 0);
        QVERIFY(!order.mismatchIndices.empty());
        QVERIFY(std::find(order.mismatchIndices.begin(), order.mismatchIndices.end(), 0) !=
                order.mismatchIndices.end());
        QVERIFY(std::find(order.mismatchIndices.begin(), order.mismatchIndices.end(), 1) !=
                order.mismatchIndices.end());
        // 第三項相對第二項是遞增的（1 -> 9），這一對不該被標記。
        QVERIFY(std::find(order.mismatchIndices.begin(), order.mismatchIndices.end(), 2) ==
                order.mismatchIndices.end());
    }

    void nodesWithoutMcidAreExcludedFromComparison() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(makeLeaf("P", 0, {}));   // 沒有 MCID：量不到內容順序
        doc.children.push_back(makeLeaf("P", 0, {0}));
        tree.roots.push_back(std::move(doc));

        const PageReadingOrder order = computePageReadingOrder(tree, 0);
        QCOMPARE(order.unknownContentOrderCount, 1);
        QCOMPARE(order.items[0].contentRank, -1);
        QCOMPARE(order.items[1].contentRank, 0);
        // 沒有內容順序可比對的項目不該被算進不一致，那會是假警報。
        QVERIFY(order.mismatchIndices.empty());
    }

    void depthSkipsAncestorsNotOnThisPage() {
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode sect;
        sect.type = "Sect";
        sect.pageIndex = -1;  // 跨頁容器，不屬於任何單一頁
        sect.children.push_back(makeLeaf("P", 0, {0}));
        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(sect));
        tree.roots.push_back(std::move(doc));

        const PageReadingOrder order = computePageReadingOrder(tree, 0);
        QCOMPARE(order.items.size(), std::size_t{1});
        // Document 與 Sect 都不屬於本頁，不該讓 P 憑空多出兩層縮排。
        QCOMPARE(order.items[0].depth, 0);
    }

    void mcidViaDescendantCountsForContainer() {
        // Table 自己沒有裸 MCID，但子節點 TR/TD 有；Table 的內容順序應該用
        // 子孫裡最小的 MCID，而不是被當成「量不到」。
        StructTree tree;
        tree.status = StructTreeStatus::Ok;
        StructNode td = makeLeaf("TD", 0, {4});
        StructNode tr;
        tr.type = "TR";
        tr.pageIndex = 0;
        tr.children.push_back(std::move(td));
        StructNode table;
        table.type = "Table";
        table.pageIndex = 0;
        table.children.push_back(std::move(tr));

        StructNode paragraph = makeLeaf("P", 0, {1});

        StructNode doc;
        doc.type = "Document";
        doc.pageIndex = -1;
        doc.children.push_back(std::move(table));
        doc.children.push_back(std::move(paragraph));
        tree.roots.push_back(std::move(doc));

        // 結構順序：Table(含 TR/TD)、TR、TD、P。內容順序：P(mcid=1) 早於 TD(mcid=4)。
        const PageReadingOrder order = computePageReadingOrder(tree, 0);
        QCOMPARE(order.unknownContentOrderCount, 0);
        QVERIFY(!order.mismatchIndices.empty());
    }
};

QTEST_APPLESS_MAIN(TestReadingOrder)
#include "test_reading_order.moc"
