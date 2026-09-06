// 頁面管理引擎層測試（WBS 5.6，PRD-PAGE-001/005/006/012）。
//
// 每一項都以「存檔 → 用另一條路徑重新開啟 → 比對每頁的文字標記」收尾。
// 只驗頁數是不夠的：順序錯掉的時候頁數往往仍然正確，那正是頁面管理最常見的 bug。

#include <QtTest>

#include <string>
#include <vector>

#include "domain/page_operations.h"
#include "engine/pages/page_editor.h"
#include "page_fixture.h"

using namespace alioth::domain::pages;
using namespace alioth::engine::pages;

namespace {

std::vector<std::string> markers(const QString& path) {
    return alioth::test::readPageMarkers(path);
}

std::vector<std::string> expectMarkers(std::initializer_list<int> oneBasedPages) {
    std::vector<std::string> out;
    for (const int page : oneBasedPages) {
        out.push_back(alioth::test::pageMarker(page).toStdString());
    }
    return out;
}

}  // namespace

class TestPageEditor : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        source_ = dir_->filePath(QStringLiteral("source.pdf"));
        target_ = dir_->filePath(QStringLiteral("target.pdf"));
        QVERIFY(alioth::test::writePdfTo(source_, alioth::test::makeMarkedPdf(5)));
    }

    void cleanup() { dir_.reset(); }

    void fixtureIsIdentifiablePerPage() {
        QCOMPARE(markers(source_), expectMarkers({1, 2, 3, 4, 5}));
    }

    void reportsMissingFile() {
        PageEditor editor;
        QVERIFY(!editor.open(dir_->filePath(QStringLiteral("no-such-file.pdf")).toStdString()));
        QVERIFY(!editor.isOpen());
        QCOMPARE(editor.pageCount(), 0);
        // 未開檔時的每個操作都要回報 NotOpen，而不是當作空文件安靜地成功。
        QCOMPARE(editor.deletePages({0}).status, PageEditStatus::NotOpen);
        QCOMPARE(editor.rotatePages({0}, PageRotation::Clockwise90).status,
                 PageEditStatus::NotOpen);
    }

    void rejectsInvalidSelectionBeforeTouchingPdfium() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));

        const PageEditResult result = editor.deletePages({1, 1});
        QCOMPARE(result.status, PageEditStatus::InvalidArgument);
        QCOMPARE(result.validation, OperationStatus::DuplicateSelection);
        // 驗證失敗不得留下任何痕跡。
        QCOMPARE(editor.pageCount(), 5);
        QVERIFY(!editor.hasStructuralChange());

        QCOMPARE(editor.deletePages({0, 1, 2, 3, 4}).validation,
                 OperationStatus::WouldEmptyDocument);
        QCOMPARE(editor.movePages({0}, 5).validation, OperationStatus::InvalidDestination);
    }

    void insertsBlankPages() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.insertBlankPages(1, 2, 300.0, 500.0).ok());
        QCOMPARE(editor.pageCount(), 7);

        const auto size = editor.pageSize(1);
        QVERIFY(size.has_value());
        QCOMPARE(size->width, 300.0);
        QCOMPARE(size->height, 500.0);

        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_),
                 (std::vector<std::string>{"PAGE-01", "", "", "PAGE-02", "PAGE-03", "PAGE-04",
                                           "PAGE-05"}));
    }

    void deletesPagesKeepingOrder() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // 刻意給非遞增順序：實作必須由大到小刪，否則會刪錯頁。
        QVERIFY(editor.deletePages({3, 1}).ok());
        QCOMPARE(editor.pageCount(), 3);
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_), expectMarkers({1, 3, 5}));
    }

    void movesPagesToNewPosition() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // [1 2 3 4 5] 把第 4、第 3 頁移到索引 1 → [1 4 3 2 5]
        QVERIFY(editor.movePages({3, 2}, 1).ok());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_), expectMarkers({1, 4, 3, 2, 5}));
    }

    // 縮圖拖曳重排的索引語意（PRD-NAV-004）。
    //
    // 這是拖放重排永遠會踩到的那個 off-by-one：Qt 的 rowsMoved 給的
    // destination 是「來源還在原位時」的插入點，而 PageEditor 的
    // destinationIndex 是移除來源之後的目標位置。往後拖時兩者差一。
    //
    // 沒有這一條，往後拖一頁會落在放開的位置**後面一格**——一個使用者
    // 每次都看得到、但很容易被當成「Qt 就是這樣」而放過的錯。
    void movingForwardLandsWhereTheUserDropped() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // [1 2 3 4 5]：把第 1 頁（索引 0）拖到第 3 頁與第 4 頁之間。
        // Qt 會給 destination = 3；主視窗換算成 destinationIndex = 2。
        QVERIFY(editor.movePages({0}, 2).ok());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();
        QCOMPARE(markers(target_), expectMarkers({2, 3, 1, 4, 5}));
    }

    void movingBackwardNeedsNoAdjustment() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        // 往前拖：Qt 的 destination 與 destinationIndex 一致，不必減一。
        QVERIFY(editor.movePages({4}, 1).ok());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();
        QCOMPARE(markers(target_), expectMarkers({1, 5, 2, 3, 4}));
    }

    void duplicatesPages() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.duplicatePages({0, 2}, 5).ok());
        QCOMPARE(editor.pageCount(), 7);
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_), expectMarkers({1, 2, 3, 4, 5, 1, 3}));
    }

    void swapsTwoPages() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.swapPages(1, 3).ok());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_), expectMarkers({1, 4, 3, 2, 5}));
    }

    void reversesPageOrder() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.reversePages().ok());
        QCOMPARE(editor.pageCount(), 5);
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_), expectMarkers({5, 4, 3, 2, 1}));
    }

    void appliesOperationVariant() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        const PageOperation operation = DeletePages{{0}};
        QVERIFY(editor.apply(operation).ok());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        QCOMPARE(markers(target_), expectMarkers({2, 3, 4, 5}));
    }

    // ---- 旋轉是文件層的修改 -----------------------------------------------

    void rotationWritesDocumentRotateEntry() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));

        const auto mediaBefore = editor.mediaBox(0);
        QVERIFY(mediaBefore.has_value());

        QVERIFY(editor.rotatePages({0}, PageRotation::Clockwise90).ok());
        // 旋轉不動頁面樹，所以不算結構性變更。
        QVERIFY(!editor.hasStructuralChange());
        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        // 原檔沒有 /Rotate，存檔結果有——證明改的是文件而不是檢視狀態。
        QVERIFY(!alioth::test::readAllBytes(source_).contains("/Rotate"));
        QVERIFY(alioth::test::readAllBytes(target_).contains("/Rotate 90"));

        PageEditor reopened;
        QVERIFY(reopened.open(target_.toStdString()));
        QCOMPARE(reopened.rotation(0), std::optional<PageRotation>{PageRotation::Clockwise90});
        QCOMPARE(reopened.rotation(1), std::optional<PageRotation>{PageRotation::None});

        // MediaBox 不受旋轉影響；受影響的是 PDFium 算出來的頁面尺寸（寬高互換）。
        const auto mediaAfter = reopened.mediaBox(0);
        QVERIFY(mediaAfter.has_value());
        QCOMPARE(mediaAfter->width(), mediaBefore->width());
        QCOMPARE(mediaAfter->height(), mediaBefore->height());
        const auto size = reopened.pageSize(0);
        QVERIFY(size.has_value());
        QCOMPARE(size->width, mediaBefore->height());
        QCOMPARE(size->height, mediaBefore->width());

        // 文字內容不受旋轉影響：頁序與標記都應該原封不動。
        reopened.close();
        QCOMPARE(markers(target_), expectMarkers({1, 2, 3, 4, 5}));
    }

    void relativeRotationAccumulates() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.rotatePages({0}, PageRotation::Clockwise90, true).ok());
        QVERIFY(editor.rotatePages({0}, PageRotation::Clockwise90, true).ok());
        QCOMPARE(editor.rotation(0), std::optional<PageRotation>{PageRotation::Half});

        // 絕對旋轉直接指定角度，不與現況疊加。
        QVERIFY(editor.rotatePages({0}, PageRotation::CounterClockwise90, false).ok());
        QCOMPARE(editor.rotation(0), std::optional<PageRotation>{PageRotation::CounterClockwise90});

        QVERIFY(editor.save(target_.toStdString()).ok());
        editor.close();

        PageEditor reopened;
        QVERIFY(reopened.open(target_.toStdString()));
        QCOMPARE(reopened.rotation(0), std::optional<PageRotation>{PageRotation::CounterClockwise90});
    }

    // ---- 存檔如實回報 ------------------------------------------------------

    void rotationOnlySavePreservesOriginalBytes() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.rotatePages({2}, PageRotation::Half).ok());

        const PageSaveResult result = editor.save(target_.toStdString());
        QVERIFY(result.ok());
        QVERIFY(!result.structural);
        // 只改頁面字典時 PDFium 能沿用原 xref，簽章因此仍然有效。
        QVERIFY2(result.signaturesPreserved(), result.save.message.c_str());
        QCOMPARE(result.save.metrics.sourceBytes, static_cast<std::uint64_t>(
                                                      QFileInfo(source_).size()));
        QVERIFY(result.save.metrics.incrementalBytes > 0);
    }

    void structuralSaveReportsFullRewriteHonestly() {
        PageEditor editor;
        QVERIFY(editor.open(source_.toStdString()));
        QVERIFY(editor.deletePages({1}).ok());
        QVERIFY(editor.hasStructuralChange());

        const PageSaveResult result = editor.save(target_.toStdString());
        QVERIFY(result.ok());
        QVERIFY(result.structural);
        // 刪頁之後 PDFium 仍然以增量形式寫出（原檔位元組完整保留在前面），
        // 這是實測結果而不是假設；哪天上游改了，這裡會先失敗而不是簽章安靜失效。
        QCOMPARE(result.save.fullRewriteFallback, false);
        QCOMPARE(result.signaturesPreserved(), true);
    }

    void newDocumentRefusesIncrementalSave() {
        PageEditor editor;
        QVERIFY(editor.createEmpty());
        QVERIFY(editor.insertBlankPages(0, 1, 200.0, 200.0).ok());

        // 沒有原始位元組可保留時，增量儲存沒有意義；要不要改走另存是呼叫端的決定。
        const PageSaveResult refused = editor.save(target_.toStdString());
        QVERIFY(!refused.ok());
        QCOMPARE(refused.save.status, alioth::engine::save::SaveStatus::SourceUnreadable);

        const PageSaveResult copied = editor.saveAsCopy(target_.toStdString());
        QVERIFY(copied.ok());
        QVERIFY(copied.save.fullRewriteFallback);
        editor.close();

        PageEditor reopened;
        QVERIFY(reopened.open(target_.toStdString()));
        QCOMPARE(reopened.pageCount(), 1);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString source_;
    QString target_;
};

QTEST_APPLESS_MAIN(TestPageEditor)
#include "test_page_editor.moc"
