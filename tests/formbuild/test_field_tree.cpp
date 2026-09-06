// Fields 面板的資料模型（PRD-FORM-021）。
//
// PDF 的表單欄位名用點分層（invoice.line1.amount），面板要把它顯示成樹。
// 這裡最容易錯的是「中介節點」——invoice 與 invoice.line1 本身可能不是實體欄位，
// 只是分組。把它們當成欄位會讓面板出現點不動的空項目，
// 而漏掉它們則會讓整棵樹塌成一層。

#include <QtTest>

#include <string>
#include <vector>

#include "engine/formbuild/field_tree.h"

using namespace alioth;
using namespace alioth::engine::formbuild;

namespace {

FieldSummary field(const std::string& name, BuildFieldType type = BuildFieldType::Text,
                   std::int32_t page = 0) {
    FieldSummary summary;
    summary.name = name;
    summary.type = type;
    summary.pageIndex = page;
    summary.rectPt = domain::RectF{10.0, 10.0, 110.0, 30.0};
    return summary;
}

const FieldTreeNode* childNamed(const std::vector<FieldTreeNode>& nodes,
                                const std::string& segment) {
    for (const FieldTreeNode& node : nodes) {
        if (node.segment == segment) return &node;
    }
    return nullptr;
}

}  // namespace

class TestFieldTree : public QObject {
    Q_OBJECT

private slots:
    void flatFieldsBecomeTopLevelNodes() {
        const auto tree = buildFieldTree({field("name"), field("email")});
        QCOMPARE(tree.size(), std::size_t{2});
        QVERIFY(tree[0].isField);
        QCOMPARE(tree[0].fullName, std::string{"name"});
        QCOMPARE(tree[1].fullName, std::string{"email"});
    }

    void dottedNamesFormAHierarchy() {
        const auto tree = buildFieldTree(
            {field("invoice.line1.amount"), field("invoice.line2.amount"), field("invoice.total")});

        QCOMPARE(tree.size(), std::size_t{1});
        const FieldTreeNode& invoice = tree.front();
        QCOMPARE(invoice.segment, std::string{"invoice"});
        // invoice 本身不是實體欄位，只是分組——當成欄位會讓面板出現點不動的項目。
        QVERIFY2(!invoice.isField, "中介節點被當成了實體欄位");
        QCOMPARE(invoice.children.size(), std::size_t{3});

        const FieldTreeNode* line1 = childNamed(invoice.children, "line1");
        QVERIFY(line1 != nullptr);
        QVERIFY(!line1->isField);
        QCOMPARE(line1->children.size(), std::size_t{1});
        QCOMPARE(line1->children.front().fullName, std::string{"invoice.line1.amount"});
        QVERIFY(line1->children.front().isField);
    }

    void fieldCountAggregatesDescendants() {
        const auto tree = buildFieldTree(
            {field("a.b.c"), field("a.b.d"), field("a.e"), field("standalone")});

        const FieldTreeNode* a = childNamed(tree, "a");
        QVERIFY(a != nullptr);
        QCOMPARE(a->fieldCount(), 3);

        const FieldTreeNode* standalone = childNamed(tree, "standalone");
        QVERIFY(standalone != nullptr);
        QCOMPARE(standalone->fieldCount(), 1);
    }

    void aNameCanBeBothFieldAndParent() {
        // 「invoice」本身是欄位，同時又有 invoice.note 這個子欄位。
        // PDF 允許這種結構，面板不能因此漏掉其中一邊。
        const auto tree = buildFieldTree({field("invoice"), field("invoice.note")});

        QCOMPARE(tree.size(), std::size_t{1});
        const FieldTreeNode& invoice = tree.front();
        QVERIFY2(invoice.isField, "同時是欄位與父節點時，欄位身分被吃掉了");
        QCOMPARE(invoice.children.size(), std::size_t{1});
        QCOMPARE(invoice.fieldCount(), 2);
    }

    void orderIsStableAndFollowsInput() {
        // 面板需要「剛建立的欄位出現在最後」這種可預期的行為。
        const auto tree = buildFieldTree({field("zebra"), field("alpha"), field("middle")});
        QCOMPARE(tree[0].segment, std::string{"zebra"});
        QCOMPARE(tree[1].segment, std::string{"alpha"});
        QCOMPARE(tree[2].segment, std::string{"middle"});
    }

    void emptyInputProducesEmptyTree() {
        QVERIFY(buildFieldTree({}).empty());
    }

    void fieldsOnPageFiltersWithoutReordering() {
        const std::vector<FieldSummary> fields{field("a", BuildFieldType::Text, 0),
                                               field("b", BuildFieldType::CheckBox, 1),
                                               field("c", BuildFieldType::Text, 0)};

        const auto page0 = fieldsOnPage(fields, 0);
        QCOMPARE(page0.size(), std::size_t{2});
        QCOMPARE(page0[0].name, std::string{"a"});
        QCOMPARE(page0[1].name, std::string{"c"});

        QCOMPARE(fieldsOnPage(fields, 1).size(), std::size_t{1});
        QVERIFY(fieldsOnPage(fields, 9).empty());
    }

    void typeAndFlagsSurviveIntoTheTree() {
        FieldSummary readOnly = field("locked", BuildFieldType::Text);
        readOnly.readOnly = true;
        readOnly.required = true;
        readOnly.widgetCount = 3;

        const auto tree = buildFieldTree({readOnly});
        QCOMPARE(tree.size(), std::size_t{1});
        QVERIFY(tree.front().readOnly);
        QVERIFY(tree.front().required);
        QCOMPARE(tree.front().widgetCount, 3);
    }

    void emptyNameIsNotTurnedIntoAPhantomNode() {
        // 空名稱的欄位在真實檔案裡存在（未命名的按鈕）。
        // 把它變成一個空白節點會讓面板出現無法辨識的項目。
        const auto tree = buildFieldTree({field(""), field("named")});
        for (const FieldTreeNode& node : tree) {
            QVERIFY2(!node.segment.empty() || node.isField,
                     "產生了既沒有名字也不是欄位的節點");
        }
    }
};

QTEST_APPLESS_MAIN(TestFieldTree)
#include "test_field_tree.moc"
