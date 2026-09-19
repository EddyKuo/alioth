// 縮圖面板（PRD-NAV-004）。
//
// 回報的症狀是「開啟 PDF 之後左側只看得到第一頁」。縮圖面板是多數人翻頁的
// 主要方式，只剩一頁等於這個面板不能用。
//
// 這支測試從 MainWindow 這一層驗，因為缺陷可能在任何一段：清單有沒有建出
// 全部的項目、項目有沒有被排到看得見的位置、縮圖請求有沒有送出去。
// 只驗 DocumentController 的話，三者都測不到。

#include <QtTest>

#include <QElapsedTimer>
#include <QListWidget>
#include <QPoint>
#include <QScrollBar>
#include <QSet>
#include <QSignalSpy>
#include <QTreeWidget>

#include "app/document_controller.h"
#include "pdf_fixture.h"
#include "ui/main_window.h"
#include "ui/page_view.h"
#include "ui/thumbnail_delegate.h"

using namespace alioth;

namespace {

// N 頁的 PDF，每頁尺寸相同。頁數刻意超過一個畫面放得下的數量，
// 這樣「只排得下一頁」與「只建出一頁」才分得開。
QByteArray makePdfWithPages(int pageCount) {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        if (i > 0) kids += " ";
        kids += QByteArray::number(i + 3) + " 0 R";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                      QByteArray::number(pageCount) + " >>");
    for (int i = 0; i < pageCount; ++i) {
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Resources << >> >>");
    }

    QByteArray pdf = "%PDF-1.7\n";
    std::vector<int> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
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

}  // namespace

class TestThumbnailPanel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // QSettings 範圍必須換掉：MainWindow 會還原上一次存下的版面，
        // 拿開發機真的存過的狀態來測，測到的是那台機器的歷史。
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("ThumbnailPanel"));
        QSettings().clear();

        file_ = test::writeTempPdf(makePdfWithPages(kPages));
        QVERIFY(file_ != nullptr);
    }

    // 清單要有 N 個項目。少於 N 代表建立那一步就錯了。
    void everyPageGetsAnItem() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QVERIFY(controller != nullptr);
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY2(list != nullptr, "找不到縮圖清單");
        QCOMPARE(list->count(), kPages);
    }

    // 每個項目都要被排到一個非空的位置上。
    //
    // 這一條與上一條分開：IconMode 的清單可以「有 N 個項目」卻把它們全部
    // 疊在 (0,0)——那時使用者看到的就是「只有第一頁」，而 count() 完全正常。
    void everyItemHasItsOwnVisibleRect() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        QCoreApplication::processEvents();

        QVERIFY(list->count() >= 2);
        const QRect first = list->visualItemRect(list->item(0));
        QVERIFY2(!first.isEmpty(), "第一個項目沒有版面尺寸");

        int overlapping = 0;
        for (int i = 1; i < list->count(); ++i) {
            const QRect rect = list->visualItemRect(list->item(i));
            if (rect.isEmpty() || rect.topLeft() == first.topLeft()) ++overlapping;
        }
        QVERIFY2(overlapping == 0,
                 qPrintable(QStringLiteral("有 %1 個縮圖與第一個疊在同一個位置——"
                                           "使用者看到的就是「只有第一頁」")
                                .arg(overlapping)));
    }

    // 換一份頁數不同的文件之後，清單要跟著換，不能留著上一份的項目。
    void reopeningRebuildsTheList() {
        auto shorter = test::writeTempPdf(makePdfWithPages(3));
        QVERIFY(shorter != nullptr);

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QCOMPARE(list->count(), kPages);

        opened.clear();
        window.openPath(shorter->fileName());
        QVERIFY(opened.wait(10000));
        QCOMPARE(list->count(), 3);
    }

    // **版面不可以因為圖示晚到而錯亂。**
    //
    // 這是使用者回報的那個缺陷：IconMode 的版面在項目**加入時**算好位置
    // 並快取，而縮圖是非同步到的。項目剛建立時只有一個頁碼、很小，
    // 圖示到了才撐成 120×160——但已經排好的位置不會重算，於是每一張縮圖
    // 都畫在當初那個小方塊的位置上，整批疊成左上角一團。
    //
    // 畫面上看起來就是「只有第一頁，其他都不見了」，而 count() 完全正常。
    // 固定格線（setGridSize）讓項目大小與圖示無關，版面因此不會變。
    void layoutSurvivesThumbnailsArrivingLate() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        // 固定格線是這個缺陷唯一能被機器判定的前提。
        //
        // 疊圖本身在 offscreen 上重現不出來：測試語料的縮圖幾乎瞬間就到，
        // 根本沒有「先小後大」那段時間差。真正看到那個畫面是用
        // alioth_uishot 開一份 40 頁的文件抓圖抓出來的（CLAUDE.md：
        // 版面問題只能用看的）。因此這裡守的是機制——項目大小不得取決於
        // 圖示到了沒有——而不是症狀。
        QVERIFY2(!list->gridSize().isEmpty(),
                 "縮圖清單沒有固定格線——項目大小會跟著圖示變，版面就會錯亂");

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));
        QCoreApplication::processEvents();

        // 關鍵性質：項目的大小與「圖示到了沒有」無關。
        //
        // 不比對絕對位置——視窗版面在開檔後還會再安定一次（停靠面板寬度、
        // 捲軸出現），那會讓整排項目合理地平移。真正壞掉的是「大小會變」，
        // 因為 IconMode 快取的是位置，大小一變就疊在一起。
        // 等圖示陸續到達——那正是會觸發重排的時刻。
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 10000) {
            if (!list->item(0)->icon().isNull()) break;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QVERIFY2(!list->item(0)->icon().isNull(), "縮圖一張都沒到，這條測不到重排");

        // 每一格都要有自己的位置。
        //
        // 缺陷發生時，項目仍然是 count() 個、大小也各自正確，只是**原點全部
        // 相同**——它們疊在左上角一團，而使用者看到的就是「只有第一頁」。
        // 這一條是那個症狀唯一能被機器判定的形式。
        QSet<QPoint> origins;
        for (int i = 0; i < list->count(); ++i) {
            origins.insert(list->visualItemRect(list->item(i)).topLeft());
        }
        QVERIFY2(origins.size() == list->count(),
                 qPrintable(QStringLiteral("%1 個縮圖只占了 %2 個位置——其餘疊在一起了")
                                .arg(list->count())
                                .arg(origins.size())));
    }

    // 捲到後面的頁也要拿得到縮圖。
    //
    // 先前只在開檔時要前 24 張，之後再也沒有人補：一份 40 頁的文件，
    // 第 25 頁之後永遠只有一個頁碼，而使用者不會知道那是還沒載入還是壞了。
    void scrollingRequestsMoreThumbnails() {
        auto longer = test::writeTempPdf(makePdfWithPages(60));
        QVERIFY(longer != nullptr);

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(longer->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        QCOMPARE(list->count(), 60);

        // 開檔時**不該**把六十張全要下來：那條 PDFium 執行緒是全行程唯一的
        // 一條，一萬頁的文件會把可見圖磚排在一萬張縮圖後面。
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        int initiallyRequested = 0;
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->icon().isNull()) ++initiallyRequested;
        }
        QVERIFY2(initiallyRequested < 60, "開檔就把整份文件的縮圖都要了");

        // 捲到最後一頁，它必須拿得到縮圖。
        list->scrollToItem(list->item(59));
        QCoreApplication::processEvents();

        QElapsedTimer timer;
        timer.start();
        while (list->item(59)->icon().isNull() && timer.elapsed() < 15000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        QVERIFY2(!list->item(59)->icon().isNull(),
                 "捲到最後一頁仍然沒有縮圖——捲動沒有補要");
    }

    // **每一頁都要真的拿到縮圖。**
    //
    // 這是使用者回報的症狀：開檔之後左側只看得到第一頁。原因是縮圖與可視區
    // 共用同一個取消權杖——開檔時排的那一批縮圖，在檢視區第一次排版
    // （開檔後幾毫秒）就被 scheduleTiles 的 cancelAll() 清光，只有已經開始
    // 渲染的那一兩張活下來。另外 discardPending(Prefetch) 也會丟掉它們，
    // 因為 Thumbnail 的優先權數值比 Prefetch 大。
    //
    // 兩條路徑都沒有錯誤訊息，所以只能靠這一條測試守。
    void everyPageActuallyGetsAThumbnail() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QVERIFY(controller != nullptr);

        QSignalSpy thumbs(controller, &app::DocumentController::thumbnailReady);
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        // 刻意在開檔後立刻動一次可視區：那正是真實情況下發生的事
        // （檢視區排版、捲動、縮放都會走 scheduleTiles）。
        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(view != nullptr);
        view->setScale(view->scale() * 1.5);
        QCoreApplication::processEvents();
        view->scrollToPage(kPages - 1);
        QCoreApplication::processEvents();

        // 全部頁面都該收到縮圖。等的是條件成立而不是固定次數的訊號——
        // 縮圖是非同步的，而且一次可能收到好幾張。
        QSet<int> pagesWithThumbnail;
        QElapsedTimer timer;
        timer.start();
        while (pagesWithThumbnail.size() < kPages && timer.elapsed() < 20000) {
            for (const QList<QVariant>& call : thumbs) {
                pagesWithThumbnail.insert(call.at(0).toInt());
            }
            if (pagesWithThumbnail.size() >= kPages) break;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        QVERIFY2(pagesWithThumbnail.size() == kPages,
                 qPrintable(QStringLiteral("只有 %1 / %2 頁拿到縮圖——"
                                           "其餘的在可視區變動時被取消了")
                                .arg(pagesWithThumbnail.size())
                                .arg(kPages)));

        // 清單上的圖示也要真的換上去，不能只是訊號發了而畫面沒動。
        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        int withIcon = 0;
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->icon().isNull()) ++withIcon;
        }
        QCOMPARE(withIcon, kPages);
        (void)withIcon;
    }

    // 點縮圖要跳到那一頁。
    //
    // 縮圖面板的存在意義就是拿來翻頁的。回報的症狀是「點了沒反應」：
    // 清單上的選取跟著滑鼠走，但檢視區停在原地。
    void clickingAThumbnailJumpsToThatPage() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(view != nullptr);
        QCoreApplication::processEvents();

        const int target = 5;
        QVERIFY(list->count() > target);
        list->scrollToItem(list->item(target));
        QCoreApplication::processEvents();

        const QRect rect = list->visualItemRect(list->item(target));
        QVERIFY(!rect.isEmpty());
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QCoreApplication::processEvents();

        QCOMPARE(list->currentRow(), target);
        QCOMPARE(static_cast<int>(view->pageIndex()), target);
    }

    // 真實動線：點過縮圖之後自己捲開，再點回原本那一格。
    //
    // 這是使用者回報「點縮圖沒反應」的實際情形。清單的選取不會跟著檢視區走，
    // 所以捲開之後選取仍停在上次點的那一格；再點它一次不會發出
    // currentRowChanged，於是完全沒有反應——而使用者做的事完全合理：
    // 「我剛剛在第 6 頁，捲遠了，點回去」。
    void clickingTheAlreadySelectedThumbnailStillJumps() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(list != nullptr && view != nullptr);
        QCoreApplication::processEvents();

        const int target = 5;
        list->scrollToItem(list->item(target));
        QCoreApplication::processEvents();
        const QRect rect = list->visualItemRect(list->item(target));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QCoreApplication::processEvents();
        QCOMPARE(static_cast<int>(view->pageIndex()), target);

        // 使用者自己捲開——捲動會換頁，但不會動到清單的選取。
        view->scrollToPage(kPages - 1);
        QCoreApplication::processEvents();
        QVERIFY(static_cast<int>(view->pageIndex()) != target);

        // 再點同一格：使用者的意圖是「回到第 6 頁」。
        list->scrollToItem(list->item(target));
        QCoreApplication::processEvents();
        const QRect again = list->visualItemRect(list->item(target));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, again.center());
        QCoreApplication::processEvents();
        QCOMPARE(static_cast<int>(view->pageIndex()), target);
    }

    // 捲動換頁時，縮圖的選取要跟著走——否則使用者看不出自己在哪一頁。
    void scrollingTheViewMovesTheThumbnailSelection() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(list != nullptr && view != nullptr);

        view->scrollToPage(kPages - 1);
        QCoreApplication::processEvents();
        QCOMPARE(list->currentRow(), static_cast<int>(view->pageIndex()));
    }

    // 可點的範圍要跟看得到的格子一樣大。
    //
    // 預設委派的項目大小取決於「圖示到了沒有」：縮圖還沒到的格子只有一行
    // 頁碼那麼大，於是使用者看到一個大格子、卻只有中間一小塊點得到。
    // 點在紙張邊緣（那正是最順手的地方）完全沒反應。
    void theWholeCellIsClickableNotJustThePaper() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(list != nullptr && view != nullptr);
        QCoreApplication::processEvents();

        const int target = 4;
        list->scrollToItem(list->item(target));
        QCoreApplication::processEvents();
        const QRect cell = list->visualItemRect(list->item(target));
        QVERIFY(!cell.isEmpty());

        // 格子要有整個格線那麼大，不是縮成圖示或文字。
        QCOMPARE(cell.size(), ui::thumbnailCellSize());

        // 點四個角落附近——那些位置都在格子裡，都該跳到這一頁。
        // 每一輪重新取一次格子的位置：選取會跟著檢視區走，而那會把清單捲動，
        // 沿用上一輪的座標測到的是別的格子。
        for (int corner = 0; corner < 4; ++corner) {
            view->scrollToPage(0);
            QCoreApplication::processEvents();
            list->scrollToItem(list->item(target));
            QCoreApplication::processEvents();

            const QRect current = list->visualItemRect(list->item(target));
            const QPoint point = corner == 0   ? current.topLeft() + QPoint(3, 3)
                                 : corner == 1 ? current.topRight() + QPoint(-3, 3)
                                 : corner == 2 ? current.bottomLeft() + QPoint(3, -3)
                                               : current.bottomRight() + QPoint(-3, -3);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QCoreApplication::processEvents();
            QVERIFY2(static_cast<int>(view->pageIndex()) == target,
                     qPrintable(QStringLiteral("點 (%1,%2) 沒跳頁——可點範圍比格子小")
                                    .arg(point.x())
                                    .arg(point.y())));
        }
    }

    // 格子大小不可以因為縮圖還沒到而不同。
    //
    // 選取框畫的就是格子。大小跟著載入狀態走的話，使用者會看到「停留過的
    // 那一頁比較大」，而其他頁的選取框只有一小條。
    void cellsAreTheSameSizeWhetherOrNotTheThumbnailArrived() {
        auto longer = test::writeTempPdf(makePdfWithPages(60));
        QVERIFY(longer != nullptr);

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(longer->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

        // 開檔不會把六十張都要下來，所以後面必然有還沒到的格子——
        // 這一條測的正是那些格子。
        int loaded = 0;
        int blank = 0;
        for (int i = 0; i < list->count(); ++i) {
            const QSize size = list->visualItemRect(list->item(i)).size();
            QCOMPARE(size, ui::thumbnailCellSize());
            if (list->item(i)->icon().isNull()) {
                ++blank;
            } else {
                ++loaded;
            }
        }
        QVERIFY2(loaded > 0 && blank > 0, "沒有同時出現「已載入」與「未載入」的格子，這條測不到差異");
    }

    // 關檔之後縮圖要跟著消失。
    //
    // 留在面板上的縮圖指向一份已經關掉的文件：點下去會跳頁，而檢視區
    // 什麼都沒有。書籤與註解清單同理——關檔後畫面上不該還有文件的內容。
    void closingTheDocumentClearsThePanels() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(list != nullptr);
        QCOMPARE(list->count(), kPages);

        QSignalSpy closed(controller, &app::DocumentController::documentClosed);
        controller->closeDocument();
        QVERIFY(closed.wait(10000));
        QCoreApplication::processEvents();

        QCOMPARE(list->count(), 0);

        auto* outline = window.findChild<QTreeWidget*>(QStringLiteral("outlineTree"));
        if (outline != nullptr) QCOMPARE(outline->topLevelItemCount(), 0);

        // 關完再開一份，清單要正確重建——清空不能把面板弄成不能再用。
        opened.clear();
        window.openPath(file_->fileName());
        QVERIFY(opened.wait(10000));
        QCOMPARE(list->count(), kPages);
    }

private:
    static constexpr int kPages = 12;
    std::unique_ptr<QTemporaryFile> file_;
};

QTEST_MAIN(TestThumbnailPanel)
#include "test_thumbnail_panel.moc"
