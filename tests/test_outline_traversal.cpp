// 書籤樹走訪（PRD-NAV-003）。
//
// 這支測試存在的原因是一個 use-after-free：原本的走訪把 std::vector 元素的位址
// 存進堆疊，同層再 push 一個兄弟就會重新配置，那個位址立刻懸空。
//
// 觸發條件是「同層有兩個以上兄弟、且非最後一個帶子節點」——幾乎所有真實文件的
// 書籤樹都長這樣，而先前的測試語料剛好都是單鏈，所以一路綠燈。
// 這裡的語料刻意做成分支型。

#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/document_controller.h"

using namespace alioth;

namespace {

// 書籤樹：
//   A（有子節點 A1、A2）
//   B（有子節點 B1）
//   C（無子節點）
// A 不是最後一個兄弟，而且帶子節點——正是會讓舊實作崩潰的形狀。
QByteArray makeBranchingOutlinePdf() {
    std::vector<QByteArray> objects;

    objects.push_back("<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>");
    for (int i = 0; i < 3; ++i) {
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] >>");
    }

    // 6: Outlines 根
    objects.push_back("<< /Type /Outlines /First 7 0 R /Last 11 0 R /Count 6 >>");
    // 7: A
    objects.push_back(
        "<< /Title (A) /Parent 6 0 R /Next 10 0 R /First 8 0 R /Last 9 0 R /Count 2 "
        "/Dest [3 0 R /Fit] >>");
    // 8: A1
    objects.push_back("<< /Title (A1) /Parent 7 0 R /Next 9 0 R /Dest [3 0 R /Fit] >>");
    // 9: A2
    objects.push_back("<< /Title (A2) /Parent 7 0 R /Prev 8 0 R /Dest [4 0 R /Fit] >>");
    // 10: B
    objects.push_back(
        "<< /Title (B) /Parent 6 0 R /Prev 7 0 R /Next 11 0 R /First 12 0 R /Last 12 0 R "
        "/Count 1 /Dest [4 0 R /Fit] >>");
    // 11: C
    objects.push_back("<< /Title (C) /Parent 6 0 R /Prev 10 0 R /Dest [5 0 R /Fit] >>");
    // 12: B1
    objects.push_back("<< /Title (B1) /Parent 10 0 R /Dest [5 0 R /Fit] >>");

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

const domain::OutlineNode* find(const std::vector<domain::OutlineNode>& nodes,
                                const std::string& title) {
    for (const domain::OutlineNode& node : nodes) {
        if (node.title == title) return &node;
    }
    return nullptr;
}

}  // namespace

class TestOutlineTraversal : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("outline.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(makeBranchingOutlinePdf());
        file.close();
    }

    void branchingTreeIsTraversedWithoutCorruption() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy outline(&controller, &app::DocumentController::outlineReady);

        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));
        QVERIFY2(outline.count() > 0 || outline.wait(10000), "書籤沒有回報");

        const auto& roots = controller.outline();
        QCOMPARE(roots.size(), std::size_t{3});

        const domain::OutlineNode* a = find(roots, "A");
        QVERIFY(a != nullptr);
        QCOMPARE(a->children.size(), std::size_t{2});
        QCOMPARE(a->children[0].title, std::string{"A1"});
        QCOMPARE(a->children[1].title, std::string{"A2"});

        // B 在 A 之後才被加入根層。舊實作在這一步讓根層的 vector 重新配置，
        // 而堆疊裡還存著 A 的 children 位址——那正是崩潰點。
        const domain::OutlineNode* b = find(roots, "B");
        QVERIFY(b != nullptr);
        QCOMPARE(b->children.size(), std::size_t{1});
        QCOMPARE(b->children[0].title, std::string{"B1"});

        const domain::OutlineNode* c = find(roots, "C");
        QVERIFY(c != nullptr);
        QVERIFY(c->children.empty());
    }

    void destinationsResolveToPageIndices() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy outline(&controller, &app::DocumentController::outlineReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));
        QVERIFY(outline.count() > 0 || outline.wait(10000));

        const auto& roots = controller.outline();
        const domain::OutlineNode* c = find(roots, "C");
        QVERIFY(c != nullptr);
        QVERIFY2(c->pageIndex.has_value(), "書籤沒有解析出目標頁");
        QCOMPARE(*c->pageIndex, 2);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_MAIN(TestOutlineTraversal)
#include "test_outline_traversal.moc"
