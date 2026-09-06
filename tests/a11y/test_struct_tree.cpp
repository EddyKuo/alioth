// 標籤結構讀取與 Tags 面板（PRD-A11Y-001、PRD-A11Y-004）。
//
// 這裡最重要的一組判定不是「有標籤時能不能畫出樹」，而是**沒有標籤時
// 面板必須說出來**。空面板會被解讀成「還沒載入」或「這份文件沒問題」，
// 而正確結論恰好相反。因此四種狀態各自有測試，而且驗的是面板實際顯示的
// 文字，不是內部旗標——使用者看到的是文字。

#include <QtTest>

#include <QLabel>
#include <QTreeWidget>

#include "engine/objects/struct_tree_reader.h"
#include "tagged_pdf_fixture.h"
#include "ui/tags_panel.h"

using namespace alioth;
using engine::objects::PdfSourceDocument;
using engine::objects::SourceStatus;
using engine::objects::StructTree;
using engine::objects::StructTreeStatus;

namespace {

StructTree readFrom(const QByteArray& bytes) {
    PdfSourceDocument source;
    const SourceStatus status =
        source.open(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (status != SourceStatus::Ok) return {};
    return engine::objects::readStructTree(source);
}

}  // namespace

class TestStructTree : public QObject {
    Q_OBJECT

private slots:
    void taggedDocumentIsParsed() {
        const StructTree tree = readFrom(test::makeTaggedPdf());
        QCOMPARE(tree.status, StructTreeStatus::Ok);
        QVERIFY2(tree.markedContent, "/MarkInfo /Marked 應為 true");
        QCOMPARE(tree.roots.size(), std::size_t{1});

        const auto& document = tree.roots.front();
        QCOMPARE(QString::fromStdString(document.type), QStringLiteral("Document"));
        QCOMPARE(document.children.size(), std::size_t{4});
        // Document + H1 + P + Figure + Figure
        QCOMPARE(tree.nodeCount, std::size_t{5});

        const auto& heading = document.children[0];
        QCOMPARE(QString::fromStdString(heading.type), QStringLiteral("H1"));
        // /T 是 UTF-8 之外的 PDFDocEncoding 位元組，解碼後要拿得回字串。
        QVERIFY(!heading.title.empty());
        // /Pg 必須換算成頁序，而不是留下物件編號。
        QCOMPARE(heading.pageIndex, 0);
    }

    void alternateTextIsExtractedAndMissingOnesAreDetected() {
        const StructTree tree = readFrom(test::makeTaggedPdf());
        QCOMPARE(tree.status, StructTreeStatus::Ok);

        const auto& document = tree.roots.front();
        const auto& withAlt = document.children[2];
        const auto& withoutAlt = document.children[3];

        QCOMPARE(QString::fromStdString(withAlt.type), QStringLiteral("Figure"));
        QVERIFY2(withAlt.hasAlternateText(), "/Alt 沒有被讀出來");
        QVERIFY(withAlt.needsAlternateText());

        QCOMPARE(QString::fromStdString(withoutAlt.type), QStringLiteral("Figure"));
        QVERIFY2(withoutAlt.needsAlternateText(), "Figure 必須被判定為需要替代文字");
        QVERIFY2(!withoutAlt.hasAlternateText(), "這個 Figure 本來就沒有 /Alt");
    }

    // 這是整個工作包最重要的一項判定。
    void untaggedDocumentIsReportedAsSuch() {
        const StructTree tree = readFrom(test::makeUntaggedPdf());
        QCOMPARE(tree.status, StructTreeStatus::NoStructTree);
        QVERIFY(tree.roots.empty());
        QVERIFY2(!tree.markedContent, "沒有 /MarkInfo 時不該回報為已標記");
    }

    // 有根卻沒有 /K 與完全沒有結構樹是兩件事：前者要補內容，後者要重新標籤。
    void emptyStructTreeIsDistinctFromNoStructTree() {
        const StructTree tree = readFrom(test::makeEmptyStructTreePdf());
        QCOMPARE(tree.status, StructTreeStatus::Malformed);
        QVERIFY(!tree.diagnostic.empty());
    }

    // PDF 是不可信任輸入：迴圈不得讓走訪停不下來。
    void cyclicStructTreeTerminates() {
        const StructTree tree = readFrom(test::makeCyclicStructTreePdf());
        // 不規定確切狀態——重點是「有回來」而且節點數有界。
        QVERIFY2(tree.nodeCount <= engine::objects::kMaxStructNodes, "節點數應有上限");
        const auto flat = engine::objects::flatten(tree);
        QVERIFY2(flat.size() < 1000, "迴圈的結構樹不該展開成大量節點");
    }

    // ---- 面板行為 ----

    void panelStatesUseDistinctMessages() {
        ui::TagsPanel panel;
        auto* summary = panel.findChild<QLabel*>(QStringLiteral("tagsSummary"));
        auto* view = panel.findChild<QTreeWidget*>(QStringLiteral("tagsTree"));
        QVERIFY(summary != nullptr);
        QVERIFY(view != nullptr);

        // 未開檔
        panel.clearDocument();
        const QString noDocument = summary->text();
        QVERIFY(!noDocument.trimmed().isEmpty());
        QCOMPARE(view->topLevelItemCount(), 0);

        // 已開檔但沒有標籤：訊息必須與「未開檔」不同，而且不得是空的。
        // 這一條就是「找不到結構樹時要明確顯示，而不是空面板」。
        panel.setStructTree(readFrom(test::makeUntaggedPdf()));
        const QString untagged = summary->text();
        QVERIFY2(!untagged.trimmed().isEmpty(), "未標籤文件必須有明確訊息，不得是空面板");
        QVERIFY2(untagged != noDocument, "未標籤與未開檔必須是不同的訊息");
        QVERIFY2(untagged.contains(QStringLiteral("沒有標籤結構")),
                 qPrintable(QStringLiteral("訊息應明說沒有標籤結構，實得：%1").arg(untagged)));
        QCOMPARE(view->topLevelItemCount(), 0);

        // 結構損毀：與「沒有標籤」也必須是不同的訊息。
        panel.setStructTree(readFrom(test::makeEmptyStructTreePdf()));
        const QString malformed = summary->text();
        QVERIFY(!malformed.trimmed().isEmpty());
        QVERIFY2(malformed != untagged, "結構損毀與完全未標籤必須可分辨");

        // 有標籤
        panel.setStructTree(readFrom(test::makeTaggedPdf()));
        QCOMPARE(view->topLevelItemCount(), 1);
        QVERIFY(summary->text() != untagged);
    }

    void panelCountsMissingAlternateText() {
        ui::TagsPanel panel;
        panel.setStructTree(readFrom(test::makeTaggedPdf()));
        QCOMPARE(panel.missingAlternateTextCount(), 1);

        auto* summary = panel.findChild<QLabel*>(QStringLiteral("tagsSummary"));
        QVERIFY(summary != nullptr);
        QVERIFY2(summary->text().contains(QStringLiteral("替代文字")),
                 qPrintable(QStringLiteral("摘要應提及替代文字缺失，實得：%1")
                                .arg(summary->text())));
    }

    // 顏色不得單獨承載意義：缺替代文字必須有文字標籤，不能只靠顏色。
    void missingAlternateTextIsLabelledInText() {
        ui::TagsPanel panel;
        panel.setStructTree(readFrom(test::makeTaggedPdf()));
        auto* view = panel.findChild<QTreeWidget*>(QStringLiteral("tagsTree"));
        QVERIFY(view != nullptr);

        bool found = false;
        QTreeWidgetItemIterator it(view);
        while (*it != nullptr) {
            if ((*it)->text(2).contains(QStringLiteral("缺少替代文字"))) found = true;
            ++it;
        }
        QVERIFY2(found, "缺替代文字的元素必須有文字標記，不可只靠顏色");
    }
};

QTEST_MAIN(TestStructTree)
#include "test_struct_tree.moc"
