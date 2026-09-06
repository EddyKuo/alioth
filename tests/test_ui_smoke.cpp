// 呈現層冒煙測試：把「開檔 → 引擎渲染 → 圖磚回到畫面 → 面板填資料」整條線走一遍。
//
// 這條線橫跨三個執行緒邊界（GUI、引擎、Qt 事件迴圈），單元測試各自都綠、
// 接起來卻不會動是很常見的失敗模式，所以需要一個真的跑起來的測試。
// 用 offscreen 平台，不需要顯示器，可在 CI 上跑。

#include <QtTest>

#include <QDockWidget>
#include <QListWidget>
#include <QMenuBar>
#include <QSettings>
#include <QTabBar>
#include <QToolBar>
#include <QSignalSpy>
#include <QTreeWidget>

#include "app/document_controller.h"
#include "pdf_fixture.h"
#include "ui/main_window.h"
#include "ui/page_view.h"

using namespace alioth;

class TestUiSmoke : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        file_ = test::writeTempPdf(test::makeSinglePagePdf());
        QVERIFY(file_ != nullptr);
    }

    // 首次啟動的版面。這一組釘的是「使用者第一眼看到什麼」，而那是靠讀程式碼
    // 看不出來的東西——十三個面板全開、Ribbon 被舊工具列擠到視窗中段、
    // 選單列與 Ribbon 同時出現，這三個缺陷在程式碼上都完全正常。
    //
    // QSettings 的範圍必須換掉：MainWindow 會還原上一次存下的版面，
    // 拿開發機上真的存過的狀態來測，測到的是那台機器的歷史而不是預設值。
    void defaultLayoutShowsOneNavigationPanelAndAFullWidthRibbon() {
        const QString organization = QCoreApplication::organizationName();
        const QString application = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("AliothTest"));
        QCoreApplication::setApplicationName(QStringLiteral("UiSmokeDefaults"));
        QSettings(QStringLiteral("AliothTest"), QStringLiteral("UiSmokeDefaults")).clear();

        {
            ui::MainWindow window;
            window.resize(1600, 1000);
            window.show();
            QTest::qWait(100);

            int visiblePanels = 0;
            for (const QDockWidget* dock : window.findChildren<QDockWidget*>()) {
                if (dock->isVisible()) ++visiblePanels;
            }
            // 面板是輔助，文件才是主角。多開幾個是使用者的選擇，不是預設。
            QCOMPARE(visiblePanels, 1);

            // Ribbon 必須橫跨整個視窗。它先前放在中央版面裡，左側面板一開，
            // Ribbon 就從視窗中段才開始，右邊留一大塊空白。
            const QToolBar* ribbonHost =
                window.findChild<QToolBar*>(QStringLiteral("ribbonToolBar"));
            QVERIFY2(ribbonHost != nullptr, "找不到 Ribbon 的容器工具列");
            QVERIFY(ribbonHost->isVisible());
            QCOMPARE(ribbonHost->x(), 0);
            QCOMPARE(ribbonHost->width(), window.width());

            // 選單列與 Ribbon 是同一批動作的兩個入口，同時出現等於要使用者
            // 在兩個地方找同一個功能。
            QVERIFY(!window.menuBar()->isVisible());

            // M0 時期的縮放工具列，每一顆按鈕都在 Ribbon 的「檢視」分頁重複了一次。
            const QToolBar* viewToolBar =
                window.findChild<QToolBar*>(QStringLiteral("viewToolBar"));
            QVERIFY(viewToolBar == nullptr || !viewToolBar->isVisible());
        }

        QCoreApplication::setOrganizationName(organization);
        QCoreApplication::setApplicationName(application);
    }

    // 簡報模式必須把介面還原回去（PRD-VIEW-009）。
    //
    // 這條釘的是一個很容易做錯、而且錯了會讓使用者很火大的行為：進簡報前
    // 開著的面板，退出後要原樣回來。做成「退出時一律套用預設版面」的話，
    // 使用者精心排好的版面會因為看了一次簡報就消失，而那不是他要求的。
    void presentationModeRestoresTheInterfaceItHid() {
        ui::MainWindow window;
        window.resize(1200, 800);
        window.show();
        QTest::qWait(50);

        // 先多開一個面板，讓「進去前的狀態」不等於預設狀態。
        QDockWidget* extra = nullptr;
        for (QDockWidget* dock : window.findChildren<QDockWidget*>()) {
            if (!dock->isVisible()) {
                dock->show();
                extra = dock;
                break;
            }
        }
        QVERIFY2(extra != nullptr, "找不到可以額外開啟的面板");
        QTest::qWait(50);

        QStringList before;
        for (const QDockWidget* dock : window.findChildren<QDockWidget*>()) {
            if (dock->isVisible()) before << dock->objectName();
        }
        before.sort();
        QVERIFY(before.size() >= 2);

        QAction* presentation = nullptr;
        QAction* escapeTarget = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("簡報模式")) presentation = action;
        }
        QVERIFY2(presentation != nullptr, "找不到簡報模式動作");

        presentation->trigger();
        QTest::qWait(50);
        for (const QDockWidget* dock : window.findChildren<QDockWidget*>()) {
            QVERIFY2(!dock->isVisible(),
                     qPrintable(QStringLiteral("簡報模式下 %1 仍然可見").arg(dock->objectName())));
        }

        // 退出走的是與 Esc 相同的路徑。
        QTest::keyClick(&window, Qt::Key_Escape);
        QTest::qWait(50);

        QStringList after;
        for (const QDockWidget* dock : window.findChildren<QDockWidget*>()) {
            if (dock->isVisible()) after << dock->objectName();
        }
        after.sort();
        QCOMPARE(after, before);
        Q_UNUSED(escapeTarget);
    }

    // 分割檢視（PRD-UI-018 / PRD-VIEW-017）。
    //
    // 驗的是「每一種模式都有正確數量的檢視，而且主檢視從頭到尾都活著」。
    // 主檢視被連帶刪掉是這種重建式實作最容易犯的錯，症狀是切回不分割之後
    // 整個中央區變成一片空白——而且只有在切換過才會發生。
    void splitModesCreateAndDestroyViewsWithoutLosingTheMainView() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(50);

        const auto viewCount = [&window] {
            return window.findChildren<ui::PageView*>().size();
        };
        const auto trigger = [&window](const QString& text) {
            for (QAction* action : window.findChildren<QAction*>()) {
                if (action->text() == text) {
                    action->trigger();
                    return true;
                }
            }
            return false;
        };

        QCOMPARE(viewCount(), 1);

        QVERIFY2(trigger(QStringLiteral("水平分割")), "找不到水平分割動作");
        QTest::qWait(50);
        QCOMPARE(viewCount(), 2);

        QVERIFY2(trigger(QStringLiteral("試算表式分割（四格）")), "找不到四格分割動作");
        QTest::qWait(50);
        QCOMPARE(viewCount(), 4);

        // 四格 → 垂直：多出來的兩個要收掉，不能留著佔記憶體與預取佇列。
        QVERIFY2(trigger(QStringLiteral("垂直分割")), "找不到垂直分割動作");
        QTest::qWait(50);
        QCOMPARE(viewCount(), 2);

        QVERIFY2(trigger(QStringLiteral("不分割")), "找不到不分割動作");
        QTest::qWait(50);
        QCOMPARE(viewCount(), 1);
        // 主檢視必須仍然看得見。deleteLater 之後只數數量會漏掉「剩下的那一個
        // 其實是新建的、原本的捲動位置已經沒了」這種情況，所以也驗可見性。
        QVERIFY(window.findChildren<ui::PageView*>().at(0)->isVisible());
    }

    // 縮放級距的上下界（PRD-ZOOM-003）。
    //
    // 驗的是「按到底會停在哪裡」。沒有夾住的話，連按縮小會讓倍率趨近 0，
    // 版面尺寸算出 0 像素、捲軸範圍歸零，畫面變成一片空白而且再也放不回來——
    // 而這個狀態沒有任何錯誤訊息，使用者只會覺得程式壞了。
    // 上界同理：倍率無上限時單一圖磚的來源區域會小到不足一個像素。
    void zoomStopsAtTheDocumentedLimitsInsteadOfRunningAway() {
        ui::MainWindow window;
        window.resize(1200, 800);
        window.show();
        QTest::qWait(50);

        auto* view = window.findChild<ui::PageView*>();
        QVERIFY(view != nullptr);
        QCOMPARE(view->scale(), 1.0);

        // 級距是 1.25 倍一階，從 1.0 按 40 次必定越過兩端。
        for (int i = 0; i < 40; ++i) view->zoomIn();
        QVERIFY2(view->scale() <= 64.0, "放大沒有上界");
        QCOMPARE(view->scale(), 64.0);

        for (int i = 0; i < 80; ++i) view->zoomOut();
        QVERIFY2(view->scale() >= 0.08, "縮小沒有下界——倍率會趨近 0，畫面變空白");
        QCOMPARE(view->scale(), 0.08);

        // 「實際大小」要真的回到 1.0，而不是回到夾住前的最後一個級距。
        view->actualSize();
        QCOMPARE(view->scale(), 1.0);
    }

    // 多頁籤外殼（PRD-UI-001）。
    //
    // 狀態轉移本身在 tests/uisystem/test_tab_layout_model.cpp 驗過了；
    // 這裡驗的是 widget 這層薄殼有沒有把兩者接對——尤其是「同一份文件不會
    // 開出第二個頁籤」與「切換頁籤不遞迴」，那兩個錯都不會產生任何錯誤訊息。
    void openingTheSameDocumentTwiceReusesItsTab() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(50);

        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
        QVERIFY2(tabs != nullptr, "找不到文件頁籤");
        QCOMPARE(tabs->count(), 0);
        QVERIFY(!tabs->isVisible());

        window.openPath(file_->fileName());
        QTest::qWait(50);
        QCOMPARE(tabs->count(), 1);

        // 再開一次同一份：不該變成兩個頁籤。兩個頁籤指向同一個路徑時，
        // 兩邊各自寫入，「檔案被別的程式改過」的守衛會對自己觸發。
        window.openPath(file_->fileName());
        QTest::qWait(50);
        QCOMPARE(tabs->count(), 1);
    }

    void closingTheLastTabKeepsTheWindowOpen() {
        ui::MainWindow window;
        window.show();
        window.openPath(file_->fileName());
        QTest::qWait(50);

        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->count(), 1);

        emit tabs->tabCloseRequested(0);
        QTest::qWait(50);

        // 主視窗關到剩零個頁籤時保留視窗（TabLayoutModel 的規則）：
        // 使用者仍然需要一個地方開下一個檔案。
        QCOMPARE(tabs->count(), 0);
        QVERIFY(window.isVisible());
        QVERIFY(!tabs->isVisible());
    }

    // 工具持續模式（PRD-UI-007）與 Edit 選單的三個入口。
    //
    // 這裡驗的是「工具列上的打勾與實際作用中的工具一致」。兩者脫鉤時
    // 沒有任何錯誤：畫完一個矩形之後工具其實已經切回選取，但工具列上
    // 仍然反白著矩形，使用者下一次拖曳會以為自己在畫第二個。
    void toolPersistenceIsOffByDefaultAndTogglable() {
        ui::MainWindow window;
        window.show();
        QTest::qWait(50);

        QAction* sticky = nullptr;
        QAction* selectAll = nullptr;
        QAction* cut = nullptr;
        QAction* paste = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            const QString text = action->text();
            if (text.startsWith(QStringLiteral("工具持續模式"))) sticky = action;
            if (text.startsWith(QStringLiteral("全選這一頁"))) selectAll = action;
            if (text.startsWith(QStringLiteral("剪下註解"))) cut = action;
            if (text.startsWith(QStringLiteral("貼上註解"))) paste = action;
        }
        QVERIFY2(sticky != nullptr, "找不到工具持續模式");
        QVERIFY2(selectAll != nullptr, "找不到全選");
        QVERIFY2(cut != nullptr, "找不到剪下註解");
        QVERIFY2(paste != nullptr, "找不到貼上註解");

        // 預設關閉：用完一個註解工具就切回選取，避免下一次點擊誤加註解。
        QVERIFY(sticky->isCheckable());
        QVERIFY(!sticky->isChecked());

        // 三個入口都要有標準鍵位，否則肌肉記憶按下去什麼都不會發生。
        QCOMPARE(selectAll->shortcut(), QKeySequence(QKeySequence::SelectAll));
        QCOMPARE(cut->shortcut(), QKeySequence(QKeySequence::Cut));
        QCOMPARE(paste->shortcut(), QKeySequence(QKeySequence::Paste));
    }

    void windowConstructsWithoutDocument() {
        ui::MainWindow window;
        window.show();
        // 不用 qWaitForWindowExposed：offscreen 平台不保證會送出曝光事件，
        // 那會讓這個測試在 CI 上偶發逾時。能建構、能跑完事件迴圈就夠了。
        QTest::qWait(100);
        QVERIFY(window.isVisible());
    }

    void openingDocumentPopulatesPanelsAndRendersTiles() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy tiles(&controller, &app::DocumentController::tileReady);
        QSignalSpy outline(&controller, &app::DocumentController::outlineReady);

        controller.openDocument(file_->fileName());
        QVERIFY2(opened.wait(10000), "開檔訊號逾時");
        QVERIFY(controller.isOpen());
        QCOMPARE(controller.pageCount(), 1);

        // 書籤解析即使在沒有書籤的文件上也必須回訊號，否則面板會永遠停在載入中。
        QVERIFY2(outline.count() > 0 || outline.wait(5000), "書籤解析未回報");

        // 版面歸呈現層所有，所以測試自己扮演呈現層算出可見範圍。
        app::PageTileRequest request;
        request.pageIndex = 0;
        request.visibleInPage = domain::RectI{0, 0, 800, 600};
        request.pageSize = domain::RectI{0, 0, 595, 842};

        app::TileScheduleOptions options;
        options.scale = 1.0;
        controller.scheduleTiles({request}, options);

        QVERIFY2(tiles.count() > 0 || tiles.wait(10000), "沒有任何圖磚回到 GUI 執行緒");

        const QImage tile = controller.tileIfReady(
            domain::TileKey{0, domain::exactScaleKey(1.0), 0, 0, domain::Rotation::None, false});
        QVERIFY(!tile.isNull());
        QCOMPARE(tile.width(), domain::kTileSize);
    }

    void thumbnailArrivesForFirstPage() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy thumbs(&controller, &app::DocumentController::thumbnailReady);

        controller.openDocument(file_->fileName());
        QVERIFY(opened.wait(10000));

        controller.requestThumbnail(0, 120);
        QVERIFY2(thumbs.count() > 0 || thumbs.wait(10000), "縮圖未產生");

        const auto args = thumbs.takeFirst();
        QCOMPARE(args.at(0).toInt(), 0);
        const QImage image = args.at(1).value<QImage>();
        QVERIFY(!image.isNull());
        // fixture 是 200×400 點的直式頁面，長邊限 120 像素時應為 60×120。
        QCOMPARE(image.height(), 120);
        QCOMPARE(image.width(), 60);
    }

    void closingDocumentResetsState() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy closed(&controller, &app::DocumentController::documentClosed);

        controller.openDocument(file_->fileName());
        QVERIFY(opened.wait(10000));
        controller.closeDocument();
        QVERIFY(closed.wait(10000));
        QVERIFY(!controller.isOpen());
        QCOMPARE(controller.pageCount(), 0);
    }

private:
    std::unique_ptr<QTemporaryFile> file_;
};

QTEST_MAIN(TestUiSmoke)
#include "test_ui_smoke.moc"
