// 選取註解工具（對標 PDF-XChange 的 Select Comments Tool）。
//
// 在這個工具出現之前，頁面上的註解**完全點不到**：命中測試只認便利貼，
// 而且是掛在「選取文字」工具上的順帶通知。其餘所有標記（螢光筆、矩形、
// 圖章、量測）只能從清單面板點選——使用者在頁面上看到一個標記、想知道
// 它是誰寫的，唯一的辦法是回到清單裡自己找。
//
// 這支測試守的是呈現層那一半：工具切換、強調框的設定與清除、
// 以及「選取註解工具不碰文字選取」。命中測試與清單同步屬於應用層，
// 由 MainWindow 承擔，這裡驗的是它有沒有拿到該有的訊號。

#include <QtTest>

#include <QSignalSpy>

#include <memory>

#include "app/document_controller.h"
#include "app/selection_controller.h"
#include "ui/page_view.h"

using namespace alioth;
using alioth::ui::PageView;
using alioth::ui::Tool;

class TestCommentSelectionTool : public QObject {
    Q_OBJECT

private slots:
    void init() {
        controller_ = std::make_unique<app::DocumentController>();
        selection_ = std::make_unique<app::SelectionController>();
        view_ = std::make_unique<PageView>(controller_.get(), selection_.get());
        view_->resize(800, 600);
    }

    void cleanup() {
        view_.reset();
        selection_.reset();
        controller_.reset();
    }

    void toolIsSelectableAndDistinctFromTextSelection() {
        QCOMPARE(view_->tool(), Tool::Select);
        view_->setTool(Tool::SelectComment);
        QCOMPARE(view_->tool(), Tool::SelectComment);
        // 與選取文字是兩個工具，不是同一個工具的兩種模式。混在一起的話，
        // 使用者就沒有辦法從一段已標記的文字開始選字。
        QVERIFY(Tool::SelectComment != Tool::Select);
    }

    // 強調框是這個工具唯一的視覺回饋。沒有它，使用者按下「下一則註解」
    // 之後只會看到畫面跳了一頁，不知道是哪一則被選中。
    void highlightRectIsSetAndCleared() {
        QCOMPARE(view_->highlightedPage(), -1);

        view_->setHighlightedRect(3, domain::RectF{10.0, 20.0, 110.0, 60.0});
        QCOMPARE(view_->highlightedPage(), 3);

        view_->clearHighlightedRect();
        QCOMPARE(view_->highlightedPage(), -1);
    }

    // 矩形由應用層提供——PageView 不認識註解。這條規則在這個工具上也不破例，
    // 因為命中測試需要註解清單，而那是 DocumentController 才有的東西。
    void pageViewDoesNotResolveAnnotationsItself() {
        view_->setTool(Tool::SelectComment);
        // 沒有人餵矩形進來時就是沒有強調框，不會自己去猜一個。
        QCOMPARE(view_->highlightedPage(), -1);
    }

    // 沒有開檔時切換工具、設定強調框都不可以崩潰：使用者可以在空視窗上
    // 先選好工具再開檔，而那是最容易被漏測的順序。
    void worksWithoutAnOpenDocument() {
        view_->setTool(Tool::SelectComment);
        view_->setHighlightedRect(0, domain::RectF{0.0, 0.0, 10.0, 10.0});
        view_->show();
        QVERIFY(QTest::qWaitForWindowExposed(view_.get()));
        QCoreApplication::processEvents();
        QCOMPARE(view_->highlightedPage(), 0);
    }

private:
    std::unique_ptr<app::DocumentController> controller_;
    std::unique_ptr<app::SelectionController> selection_;
    std::unique_ptr<PageView> view_;
};

QTEST_MAIN(TestCommentSelectionTool)
#include "test_comment_selection_tool.moc"
