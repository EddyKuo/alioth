// 替代文字寫入通道測試（PRD-A11Y-004，ADR-002）。

#include <QtTest>

#include "../a11y/tagged_pdf_fixture.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/struct_alt_text_writer.h"
#include "engine/objects/struct_tree_reader.h"

using namespace alioth::engine::objects;

namespace {

// makeTaggedPdf() 的物件配置（見 tests/a11y/tagged_pdf_fixture.h）：
// 9 = Figure，已有 /Alt；10 = Figure，沒有 /Alt。
constexpr int kFigureWithAlt = 9;
constexpr int kFigureWithoutAlt = 10;
constexpr int kContentsObject = 4;  // 有物件編號，但不是結構元素，用來測型別檢查

[[nodiscard]] const StructNode* findByObjectNumber(const std::vector<StructNode>& roots,
                                                   int objectNumber) {
    for (const StructNode& node : roots) {
        if (node.objectNumber == objectNumber) return &node;
        if (const StructNode* found = findByObjectNumber(node.children, objectNumber)) return found;
    }
    return nullptr;
}

}  // namespace

class TestStructAltTextWriter : public QObject {
    Q_OBJECT

private slots:
    void setsAltTextOnFigureWithoutOne() {
        IncrementalAppender appender;
        std::string diagnostic;
        QCOMPARE(static_cast<int>(appender.open(alioth::test::toStdString(
                                       alioth::test::makeTaggedPdf()), &diagnostic)),
                 static_cast<int>(SourceStatus::Ok));

        const AltTextEditStatus status =
            setAlternateText(appender, kFigureWithoutAlt, "\xE6\xB5\x81\xE7\xA8\x8B\xE7\xB5\x90\xE6\x9E\x9C\xE5\x9C\x96");
        QVERIFY2(status.ok, status.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        PdfSourceDocument reparsed;
        QCOMPARE(static_cast<int>(reparsed.open(built.bytes)), static_cast<int>(SourceStatus::Ok));
        const StructTree tree = readStructTree(reparsed);
        QCOMPARE(static_cast<int>(tree.status), static_cast<int>(StructTreeStatus::Ok));

        const StructNode* node = findByObjectNumber(tree.roots, kFigureWithoutAlt);
        QVERIFY(node != nullptr);
        QVERIFY(node->hasAlternateText());
        QCOMPARE(QString::fromStdString(node->altText),
                 QString::fromUtf8("\xE6\xB5\x81\xE7\xA8\x8B\xE7\xB5\x90\xE6\x9E\x9C\xE5\x9C\x96"));
    }

    void emptyTextRemovesExistingAlt() {
        IncrementalAppender appender;
        QCOMPARE(static_cast<int>(appender.open(alioth::test::toStdString(alioth::test::makeTaggedPdf()))),
                 static_cast<int>(SourceStatus::Ok));

        const AltTextEditStatus status = setAlternateText(appender, kFigureWithAlt, "");
        QVERIFY2(status.ok, status.diagnostic.c_str());

        const BuildResult built = appender.build();
        QVERIFY(built.ok);

        PdfSourceDocument reparsed;
        QCOMPARE(static_cast<int>(reparsed.open(built.bytes)), static_cast<int>(SourceStatus::Ok));
        const StructTree tree = readStructTree(reparsed);
        const StructNode* node = findByObjectNumber(tree.roots, kFigureWithAlt);
        QVERIFY(node != nullptr);
        // 移除鍵值而不是寫入空字串：hasAlternateText() 必須確實變回 false。
        QVERIFY(!node->hasAlternateText());
        QVERIFY(node->needsAlternateText());
    }

    void rejectsDirectObjectWithZeroNumber() {
        IncrementalAppender appender;
        QCOMPARE(static_cast<int>(appender.open(alioth::test::toStdString(alioth::test::makeTaggedPdf()))),
                 static_cast<int>(SourceStatus::Ok));
        const AltTextEditStatus status = setAlternateText(appender, 0, "文字");
        QVERIFY(!status.ok);
        QVERIFY(!status.diagnostic.empty());
    }

    void rejectsObjectNumberNotInFile() {
        IncrementalAppender appender;
        QCOMPARE(static_cast<int>(appender.open(alioth::test::toStdString(alioth::test::makeTaggedPdf()))),
                 static_cast<int>(SourceStatus::Ok));
        const AltTextEditStatus status = setAlternateText(appender, 9999, "文字");
        QVERIFY(!status.ok);
    }

    void rejectsObjectThatIsNotAStructElement() {
        IncrementalAppender appender;
        QCOMPARE(static_cast<int>(appender.open(alioth::test::toStdString(alioth::test::makeTaggedPdf()))),
                 static_cast<int>(SourceStatus::Ok));
        const AltTextEditStatus status = setAlternateText(appender, kContentsObject, "文字");
        QVERIFY(!status.ok);
    }
};

QTEST_APPLESS_MAIN(TestStructAltTextWriter)
#include "test_struct_alt_text_writer.moc"
