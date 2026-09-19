// 寫入之後的重載（UI-01 / UI-02 / UI-03）。
//
// 這個架構下每一次文件修改都當下寫進磁碟，然後重新讀取檔案才看得到結果。
// 重載走的若是「開新檔」那條路，呈現層收到的訊號語意就是「換了一份文件」——
// 檢視區跳回第 1 頁、縮放重設、縮圖捲回頂端、註解清單重來。
// 使用者在第 25 頁畫一個矩形，畫面回到第 1 頁。
//
// 這支測試從 MainWindow 這一層驗，因為缺陷正好落在「控制器發哪個訊號」與
// 「呈現層怎麼解讀它」之間——兩邊各自的單元測試都不會發現。

#include <QtTest>

#include <QAction>
#include <QElapsedTimer>
#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollBar>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTimer>

#include "app/document_controller.h"
#include "app/selection_controller.h"
#include "pdf_fixture.h"
#include "text/text_pdf_fixture.h"
#include "ui/main_window.h"
#include "ui/page_view.h"
#include "ui/ribbon/action_registry.h"

using namespace alioth;

namespace {

// N 頁的 PDF，每頁尺寸相同、每頁都有內容串流。頁數要多到「第 33 頁之後」
// 這件事測得出來。
//
// 內容串流是必要的而不是裝飾：沒有 /Contents 的頁面在註解寫入路徑上會被
// 判為不完整，寫入失敗並跳出一個 modal 對話框——測試於是停住而不是失敗。
QByteArray makePdfWithPages(int pageCount) {
    const QByteArray content = "0.5 0.5 0.5 rg\n20 20 100 80 re\nf\n";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        if (i > 0) kids += " ";
        kids += QByteArray::number(3 + i * 2) + " 0 R";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                      QByteArray::number(pageCount) + " >>");
    for (int i = 0; i < pageCount; ++i) {
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 600] /Resources << >> "
                          "/Contents " + QByteArray::number(4 + i * 2) + " 0 R >>");
        objects.push_back("<< /Length " + QByteArray::number(content.size()) +
                          " >>\nstream\n" + content + "endstream");
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

// 跳出來的 modal 對話框會讓 offscreen 測試整個停住（而不是失敗），症狀是
// 看門狗在幾分鐘後把行程殺掉，什麼都看不到。這個看守每 50 毫秒巡一次，
// 把對話框的文字記下來並關掉它——這樣「意外跳出確認框」會變成一條有訊息的
// 失敗，而不是一次逾時。
class ModalWatcher : public QObject {
public:
    explicit ModalWatcher(QObject* parent = nullptr) : QObject(parent) {
        timer_.setInterval(50);
        connect(&timer_, &QTimer::timeout, this, [this] {
            QWidget* modal = QApplication::activeModalWidget();
            if (modal == nullptr) return;
            QString text = modal->windowTitle();
            if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                text += QStringLiteral(" / ") + box->text() + QStringLiteral(" / ") +
                        box->informativeText();
            }
            seen_ << text;
            modal->close();
        });
        timer_.start();
    }

    [[nodiscard]] QStringList seen() const { return seen_; }

private:
    QTimer timer_;
    QStringList seen_;
};

// 寫進一個真的目錄，不用 QTemporaryFile。
//
// QTemporaryFile 會一直握著檔案控制代碼，而所有寫入路徑最後都是
// 「寫暫存 → 原子更名」——更名蓋不過一個被握著的檔案，於是寫入失敗並跳出
// 「寫入失敗」對話框。這裡踩過一次，錯誤訊息與註解內容完全無關。
QString writePdfInto(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    file.write(bytes);
    file.close();
    return path;
}

// 等到條件成立，而不是等一段固定的時間。
//
// 固定延遲在單獨跑的時候剛好夠，在 ctest -j 下就不夠——而那種失敗只會
// 偶爾出現，看起來像環境問題。等的是「非同步結果到了沒有」，那就該直接
// 問那個條件（CLAUDE.md：平行執行才失敗的測試要當成缺陷處理）。
template <typename Predicate>
bool waitFor(Predicate predicate, int budgetMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > budgetMs) return false;
        QTest::qWait(25);
    }
    return true;
}

}  // namespace

class TestUiReload : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // MainWindow 會還原上次存下的版面與工作階段。拿開發機真的存過的狀態
        // 來測，測到的是那台機器的歷史。
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("UiReload"));
        QSettings().clear();
    }

    // 加一則註解之後，檢視區要停在原來那一頁、維持原來的倍率。
    //
    // 這是使用者回報「操作流程有問題」最主要的來源：所有寫入都會重載，
    // 而重載若被當成開新檔，每一次註解都把人丟回第 1 頁。
    void writingAnAnnotationKeepsThePageAndZoom() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePdfInto(dir, QStringLiteral("reload.pdf"), makePdfWithPages(40));
        QVERIFY(!path.isEmpty());

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        ModalWatcher watcher;

        auto* controller = window.findChild<app::DocumentController*>();
        QVERIFY(controller != nullptr);
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(path);
        QVERIFY(opened.wait(10000));

        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(view != nullptr);
        QCoreApplication::processEvents();

        constexpr int kTarget = 25;
        view->setPageIndex(kTarget);
        view->setScale(1.75);
        QCoreApplication::processEvents();
        QCOMPARE(static_cast<int>(view->pageIndex()), kTarget);
        const double scaleBefore = view->scale();

        // 走使用者真正走的那條路：選矩形工具、在頁面上框一塊。
        QSignalSpy reloaded(controller, &app::DocumentController::documentReloaded);
        view->setTool(ui::Tool::Rectangle);
        emit view->shapeDrawn(kTarget, domain::RectF{50.0, 100.0, 200.0, 250.0});
        QVERIFY2(reloaded.wait(15000),
                 qPrintable(QStringLiteral("寫入之後沒有重載；跳出的對話框：[%1]")
                                .arg(watcher.seen().join(QStringLiteral(" | ")))));
        QCoreApplication::processEvents();

        QCOMPARE(static_cast<int>(view->pageIndex()), kTarget);
        QVERIFY2(std::abs(view->scale() - scaleBefore) < 1e-9,
                 qPrintable(QStringLiteral("縮放從 %1 被重設成 %2")
                                .arg(scaleBefore)
                                .arg(view->scale())));
    }

    // 重載不可以重建縮圖清單。
    //
    // 重建會把使用者捲到的位置與選取一起丟掉，而他只是加了一則註解。
    void writingAnAnnotationDoesNotRebuildTheThumbnailList() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePdfInto(dir, QStringLiteral("reload.pdf"), makePdfWithPages(40));
        QVERIFY(!path.isEmpty());

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(path);
        QVERIFY(opened.wait(10000));

        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        auto* list = window.findChild<QListWidget*>(QStringLiteral("thumbnailList"));
        QVERIFY(view != nullptr && list != nullptr);
        QCoreApplication::processEvents();

        constexpr int kTarget = 25;
        view->setPageIndex(kTarget);
        QCoreApplication::processEvents();
        QCOMPARE(list->currentRow(), kTarget);
        const int scrollBefore = list->verticalScrollBar()->value();
        QVERIFY2(scrollBefore > 0, "縮圖清單沒有捲動，這條測不到「捲動位置被丟掉」");

        QSignalSpy reloaded(controller, &app::DocumentController::documentReloaded);
        view->setTool(ui::Tool::Rectangle);
        emit view->shapeDrawn(kTarget, domain::RectF{50.0, 100.0, 200.0, 250.0});
        QVERIFY(reloaded.wait(15000));
        QCoreApplication::processEvents();

        QCOMPARE(list->count(), 40);
        QCOMPARE(list->currentRow(), kTarget);
        QCOMPARE(list->verticalScrollBar()->value(), scrollBefore);
    }

    // **第 33 頁之後的註解也要進得了註解清單。**
    //
    // 先前只在開檔時掃前 33 頁，之後再也沒有人補——而那份清單同時是
    // 上一則／下一則、頁面上的註解命中測試、匯出選定註解的唯一資料來源。
    // 一份 80 頁的文件，第 60 頁的便利貼在清單裡看不到、點下去也開不了。
    void annotationsOnLatePagesReachTheCommentList() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePdfInto(dir, QStringLiteral("late.pdf"), makePdfWithPages(80));
        QVERIFY(!path.isEmpty());

        // 看守留著：它同時是「不該跳對話框」的斷言（見函式結尾）。
        ModalWatcher watcher;
        constexpr int kLatePage = 60;
        {
            // 先在第 60 頁放一則註解，然後把視窗關掉——下一段要驗的是
            // 「重新開啟之後能不能找到它」。
            ui::MainWindow window;
            window.resize(1280, 860);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));

            auto* controller = window.findChild<app::DocumentController*>();
            QSignalSpy opened(controller, &app::DocumentController::documentOpened);
            window.openPath(path);
            QVERIFY(opened.wait(10000));

            auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
            QVERIFY(view != nullptr);
            view->setPageIndex(kLatePage);
            QCoreApplication::processEvents();

            QSignalSpy reloaded(controller, &app::DocumentController::documentReloaded);
            view->setTool(ui::Tool::Rectangle);
            emit view->shapeDrawn(kLatePage, domain::RectF{50.0, 100.0, 200.0, 250.0});
            QVERIFY2(reloaded.wait(15000), "第 60 頁的註解沒有寫進去");
            QCoreApplication::processEvents();
        }

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(path);
        QVERIFY(opened.wait(10000));

        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(view != nullptr);
        // 捲到那一頁，清單就該補上它。
        view->setPageIndex(kLatePage);
        // 用 QTest::qWait 而不是自己轉 processEvents 迴圈。
        //
        // 熱迴圈會把 CPU 佔滿，而真正要等的是 PDFium 那條**唯一**的工作
        // 執行緒把 33 頁的註解掃回來——搶著空轉只會讓它更慢。這一條先前
        // 就是這樣在整檔連跑時逾時，單獨跑卻 1.3 秒就過。
        const bool found = waitFor([controller] {
            for (const domain::AnnotationSummary& item : controller->annotations()) {
                if (item.pageIndex == kLatePage) return true;
            }
            return false;
        });
        QVERIFY2(found, qPrintable(QStringLiteral("捲到第 60 頁之後，那一頁的註解仍然不在"
                                                  "清單裡；跳出的對話框：[%1]")
                                       .arg(watcher.seen().join(QStringLiteral(" | ")))));

        auto* list = window.findChild<QListWidget*>(QStringLiteral("annotationList"));
        QVERIFY(list != nullptr);
        const QString needle = QStringLiteral("第 %1 頁").arg(kLatePage + 1);
        waitFor([list, &needle] {
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->text().contains(needle)) return true;
            }
            return false;
        });
        bool listed = false;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().contains(QStringLiteral("第 %1 頁").arg(kLatePage + 1))) {
                listed = true;
            }
        }
        QVERIFY2(listed, "註解進了模型卻沒有進清單面板");
        QVERIFY2(watcher.seen().isEmpty(),
                 qPrintable(QStringLiteral("跳出了非預期的對話框：[%1]")
                                .arg(watcher.seen().join(QStringLiteral(" | ")))));
    }

    // 跨頁螢光筆要在**兩頁**各留一則。
    //
    // 先前那段迴圈持有 SelectionController 內部那一份的參照，而迴圈裡的
    // 寫入會同步重載並 clearSelection()——第二圈迭代的是一塊已經釋放的
    // 記憶體。release 建置下的症狀是「第二頁沒有標記」，沒有錯誤訊息。
    void crossPageHighlightMarksBothPages() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePdfInto(dir, QStringLiteral("text.pdf"), test::makeTextPdf());
        QVERIFY(!path.isEmpty());

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        ModalWatcher watcher;

        auto* controller = window.findChild<app::DocumentController*>();
        auto* selection = window.findChild<app::SelectionController*>();
        auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
        QVERIFY(controller != nullptr && selection != nullptr && registry != nullptr);

        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(path);
        QVERIFY(opened.wait(10000));
        QCoreApplication::processEvents();

        QSignalSpy changed(selection, &app::SelectionController::selectionChanged);
        selection->beginSelection(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));
        changed.clear();
        selection->extendSelection(1, domain::PointF{200.0, 404.0});
        QVERIFY(changed.wait(10000));
        QVERIFY2(selection->selection().isMultiPage(), "沒有選到跨頁，這條測不到那個缺陷");

        QAction* highlight = registry->action(QStringLiteral("annot.highlightSelection"));
        QVERIFY2(highlight != nullptr, "螢光筆動作沒有註冊");

        QSignalSpy reloaded(controller, &app::DocumentController::documentReloaded);
        highlight->trigger();
        QVERIFY2(reloaded.wait(15000),
                 qPrintable(QStringLiteral("第一次寫入沒有重載；跳出的對話框：[%1]")
                                .arg(watcher.seen().join(QStringLiteral(" | ")))));
        // 兩頁各一次寫入，各觸發一次重載；等第二次。
        if (reloaded.count() < 2) reloaded.wait(15000);
        const auto markedPages = [controller] {
            bool first = false;
            bool second = false;
            for (const domain::AnnotationSummary& item : controller->annotations()) {
                if (item.subtype != "Highlight") continue;
                if (item.pageIndex == 0) first = true;
                if (item.pageIndex == 1) second = true;
            }
            return std::pair<bool, bool>{first, second};
        };
        waitFor([&markedPages] {
            const auto [first, second] = markedPages();
            return first && second;
        });
        QVERIFY2(watcher.seen().isEmpty(),
                 qPrintable(QStringLiteral("跳出了非預期的對話框：[%1]")
                                .arg(watcher.seen().join(QStringLiteral(" | ")))));

        const auto [onFirst, onSecond] = markedPages();
        QVERIFY2(onFirst, "第一頁沒有螢光筆");
        QVERIFY2(onSecond, "第二頁沒有螢光筆——跨頁選取的第二圈迭代到已釋放的記憶體");
    }
    // 選一則註解，屬性面板要顯示它。
    //
    // 面板本身（顏色、不透明度、旗標的編輯）早就完整並有自己的測試，
    // 但從來沒有人餵資料給它，也沒有人接它的 annotationEdited——
    // Ctrl+' 叫出來的永遠是一句「選取一則註解以檢視並修改它的屬性」。
    void selectingAnAnnotationFillsThePropertiesPanel() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePdfInto(dir, QStringLiteral("props.pdf"), makePdfWithPages(5));
        QVERIFY(!path.isEmpty());

        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ModalWatcher watcher;

        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(path);
        QVERIFY(opened.wait(10000));

        auto* view = window.findChild<ui::PageView*>(QStringLiteral("pageView"));
        QVERIFY(view != nullptr);

        // 面板要先看得到：收起來時刻意不讀檔（讀的是整份文件）。
        auto* dock = window.findChild<QDockWidget*>(QStringLiteral("annotationPropertiesDock"));
        QVERIFY2(dock != nullptr, "找不到註解屬性面板");
        dock->show();
        QTest::qWait(50);

        QSignalSpy reloaded(controller, &app::DocumentController::documentReloaded);
        view->setTool(ui::Tool::Rectangle);
        emit view->shapeDrawn(0, domain::RectF{50.0, 100.0, 220.0, 260.0});
        QVERIFY2(reloaded.wait(15000),
                 qPrintable(QStringLiteral("寫入失敗；跳出的對話框：[%1]")
                                .arg(watcher.seen().join(QStringLiteral(" | ")))));
        auto* list = window.findChild<QListWidget*>(QStringLiteral("annotationList"));
        QVERIFY(list != nullptr);
        waitFor([list] { return list->count() > 0; });
        QVERIFY2(list->count() > 0,
                 qPrintable(QStringLiteral("註解清單是空的；模型裡有 %1 則，對話框：[%2]")
                                .arg(controller->annotations().size())
                                .arg(watcher.seen().join(QStringLiteral(" | ")))));
        list->setCurrentRow(0);

        // 面板從空狀態（index 0）切到編輯器（index 1）才代表它真的收到了。
        auto* stack = window.findChild<QStackedWidget*>();
        QVERIFY2(stack != nullptr, "找不到屬性面板的堆疊");
        waitFor([stack] { return stack->currentIndex() == 1; });
        QVERIFY2(stack->currentIndex() == 1,
                 "選了一則註解，屬性面板仍然停在「選取一則註解」的空狀態");

        auto* type = window.findChild<QLabel*>(QStringLiteral("annotationTypeLabel"));
        QVERIFY(type != nullptr);
        QVERIFY2(!type->text().isEmpty(), "屬性面板的類型欄位是空的");
        QVERIFY(watcher.seen().isEmpty());
    }

    // 沒有開啟文件時，需要文件的動作一律灰掉。
    //
    // 先前三種表現並存：有的跳「尚未開啟文件」、有的按了完全沒反應、
    // 有的根本沒檢查（快速存取列的儲存與列印永遠可按）。
    void actionsThatNeedADocumentAreDisabledWithoutOne() {
        ui::MainWindow window;
        window.resize(1280, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* registry = window.findChild<ui::ribbon::ActionRegistry*>();
        QVERIFY(registry != nullptr);

        for (const char* id : {"file.save", "file.print", "file.exportText", "page.delete",
                               "annot.highlightSelection", "comment.delete", "edit.copy",
                               "view.zoomIn", "nav.nextPage"}) {
            QAction* action = registry->action(QString::fromLatin1(id));
            QVERIFY2(action != nullptr, id);
            QVERIFY2(!action->isEnabled(),
                     qPrintable(QStringLiteral("沒有文件時 %1 仍然可按").arg(
                         QString::fromLatin1(id))));
        }

        // 面板開關相反：要先叫得出面板才開得了檔案。
        for (const char* id : {"view.panel.thumbnails", "comment.list", "search.advanced"}) {
            QAction* action = registry->action(QString::fromLatin1(id));
            QVERIFY2(action != nullptr, id);
            QVERIFY2(action->isEnabled(),
                     qPrintable(QStringLiteral("沒有文件時 %1 不該被灰掉").arg(
                         QString::fromLatin1(id))));
        }

        // 開了文件之後要恢復。
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePdfInto(dir, QStringLiteral("enable.pdf"), makePdfWithPages(3));
        QVERIFY(!path.isEmpty());
        auto* controller = window.findChild<app::DocumentController*>();
        QSignalSpy opened(controller, &app::DocumentController::documentOpened);
        window.openPath(path);
        QVERIFY(opened.wait(10000));
        waitFor([registry] {
            const QAction* save = registry->action(QStringLiteral("file.save"));
            return save != nullptr && save->isEnabled();
        });

        for (const char* id : {"file.save", "file.print", "view.zoomIn", "nav.nextPage"}) {
            QAction* action = registry->action(QString::fromLatin1(id));
            QVERIFY2(action != nullptr && action->isEnabled(),
                     qPrintable(QStringLiteral("開了文件之後 %1 仍然是灰的").arg(
                         QString::fromLatin1(id))));
        }
    }
};

QTEST_MAIN(TestUiReload)
#include "test_ui_reload.moc"
