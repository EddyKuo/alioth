// 替代文字應用層服務測試（PRD-A11Y-004）：檔案層級的增量寫入、原子更名、
// 以及以截斷方式復原，三件事接起來要能動——各自的單元測試不保證這一點。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "../a11y/tagged_pdf_fixture.h"
#include "app/accessibility_service.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/objects/struct_tree_reader.h"

using namespace alioth;

namespace {

[[nodiscard]] const engine::objects::StructNode* findByObjectNumber(
    const std::vector<engine::objects::StructNode>& roots, int objectNumber) {
    for (const engine::objects::StructNode& node : roots) {
        if (node.objectNumber == objectNumber) return &node;
        if (const auto* found = findByObjectNumber(node.children, objectNumber)) return found;
    }
    return nullptr;
}

}  // namespace

class TestAccessibilityService : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("tagged.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(test::makeTaggedPdf());
        file.close();
    }

    void setsAltTextAndCanBeRevertedByTruncation() {
        app::AccessibilityService service;

        app::SetAlternateTextRequest request;
        request.path = path_;
        request.structElementObjectNumber = 10;  // 沒有 /Alt 的 Figure（見 tagged_pdf_fixture.h）
        request.altText = QStringLiteral("流程圖示意");

        const app::SetAlternateTextResult result = service.setAlternateText(request);
        QVERIFY2(result.ok, qUtf8Printable(result.message));

        {
            QFile file(path_);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            file.close();
            engine::objects::PdfSourceDocument source;
            QCOMPARE(static_cast<int>(source.open(std::string(bytes.constData(),
                                                              static_cast<std::size_t>(bytes.size())))),
                     static_cast<int>(engine::objects::SourceStatus::Ok));
            const engine::objects::StructTree tree = engine::objects::readStructTree(source);
            const auto* node = findByObjectNumber(tree.roots, 10);
            QVERIFY(node != nullptr);
            QCOMPARE(QString::fromStdString(node->altText), QStringLiteral("流程圖示意"));
        }

        QString message;
        const bool reverted =
            service.revertAppend(path_, result.previousSize, result.boundaryGuard, &message);
        QVERIFY2(reverted, qUtf8Printable(message));

        {
            QFile file(path_);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            file.close();
            QCOMPARE(static_cast<quint64>(bytes.size()), result.previousSize);
            engine::objects::PdfSourceDocument source;
            QCOMPARE(static_cast<int>(source.open(std::string(bytes.constData(),
                                                              static_cast<std::size_t>(bytes.size())))),
                     static_cast<int>(engine::objects::SourceStatus::Ok));
            const engine::objects::StructTree tree = engine::objects::readStructTree(source);
            const auto* node = findByObjectNumber(tree.roots, 10);
            QVERIFY(node != nullptr);
            QVERIFY(!node->hasAlternateText());
        }
    }

    void rejectsMissingFile() {
        app::AccessibilityService service;
        app::SetAlternateTextRequest request;
        request.path = dir_->filePath(QStringLiteral("does-not-exist.pdf"));
        request.structElementObjectNumber = 10;
        request.altText = QStringLiteral("x");
        const app::SetAlternateTextResult result = service.setAlternateText(request);
        QVERIFY(!result.ok);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_MAIN(TestAccessibilityService)
#include "test_accessibility_service.moc"
