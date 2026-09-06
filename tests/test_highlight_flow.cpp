// 核心迴圈的整合測試：選取文字 → 產生 QuadPoints → 寫入螢光筆 → 增量儲存。
//
// 三個子系統（文字擷取、外觀串流、增量儲存）各自的單元測試都綠，不代表接起來會動。
// 這裡驗的正是介面之間的假設是否一致——特別是座標系：文字層給的是頁面座標
// （Y 向上），外觀串流也要頁面座標，中間任何一次多餘的翻轉都會讓螢光筆
// 標在正確位置的鏡像處，而那在單元測試裡看不出來。

#include <QtTest>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/annotation_service.h"
#include "app/document_controller.h"
#include "app/selection_controller.h"
#include "engine/annotations/annotation_document.h"
#include "text/text_pdf_fixture.h"

using namespace alioth;

class TestHighlightFlow : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("highlight.pdf"));

        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        original_ = test::makeTextPdf();
        file.write(original_);
        file.close();
    }

    // 跨頁選取（PRD-TXT-002）。
    //
    // 三個重點都是「看起來像成功、實際上壞掉」的那種：
    //   一、涵蓋的頁面要齊全；
    //   二、頁與頁之間要有換行，否則前一頁末字與下一頁首字黏成一個不存在的詞；
    //   三、每一頁的 quad 都要落在自己那一頁的座標系內——把第二頁的 quad
    //       算在第一頁的座標上，畫出來是跑到頁面外的色塊而不是明顯的失敗。
    void selectionSpansPages() {
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);

        selection.beginSelection(0, domain::PointF{60.0, 404.0});
        QVERIFY2(changed.wait(10000), "起始選取沒有回報");
        changed.clear();

        // 拖到第二頁的第一行。
        selection.extendSelection(1, domain::PointF{200.0, 404.0});
        QVERIFY2(changed.wait(10000), "跨頁選取沒有回報");

        const app::Selection& current = selection.selection();
        QVERIFY2(current.isMultiPage(),
                 qPrintable(QStringLiteral("只選到 %1 頁").arg(current.pages.size())));
        QCOMPARE(current.pages.size(), std::size_t(2));
        QCOMPARE(current.pages[0].pageIndex, 0);
        QCOMPARE(current.pages[1].pageIndex, 1);

        // 摘要欄位描述第一頁，但 text 是全部串起來的——複製的人要的是完整內容。
        QCOMPARE(current.pageIndex, 0);
        QVERIFY2(current.text.contains(QLatin1Char('\n')),
                 "跨頁的文字之間沒有換行，兩頁的字會黏在一起");
        QVERIFY(current.text.contains(QStringLiteral("Alioth")));

        for (const app::PageSelection& page : current.pages) {
            QVERIFY(!page.quads.empty());
            for (const domain::QuadPoint& quad : page.quads) {
                const domain::RectF box = quad.boundingBox();
                QVERIFY2(box.left >= 0.0 && box.right <= test::kTextPageWidth,
                         qPrintable(QStringLiteral("第 %1 頁的 quad 超出頁寬")
                                        .arg(page.pageIndex)));
                QVERIFY(box.bottom >= 0.0 && box.top <= test::kTextPageHeight);
            }
        }
    }

    void backwardCrossPageSelectionWorksToo() {
        // 由後往前拖。少了這條，使用者只能單向選取，那是一半的功能。
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);

        // 起點必須落在真的有字的位置。x=200 已經超過第二頁那一行的尾端，
        // 錨點會取不到字元索引，整個選取根本不會開始。
        selection.beginSelection(1, domain::PointF{60.0, 404.0});
        QVERIFY2(changed.wait(10000), "起始選取沒有回報");
        changed.clear();

        selection.extendSelection(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));

        const app::Selection& current = selection.selection();
        QCOMPARE(current.pages.size(), std::size_t(2));
        // 頁面永遠依頁碼排序，與拖曳方向無關——不排序的話，複製出來的文字
        // 順序會跟著使用者的手勢方向倒過來。
        QCOMPARE(current.pages[0].pageIndex, 0);
        QCOMPARE(current.pages[1].pageIndex, 1);
    }

    void singlePageSelectionStillReportsOnePage() {
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);

        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));

        const app::Selection& current = selection.selection();
        QVERIFY(!current.isMultiPage());
        QCOMPARE(current.pages.size(), std::size_t(1));
        QCOMPARE(current.text, QStringLiteral("Hello"));
    }

    void selectionProducesQuadsOnThePage() {
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);

        // 第一行文字的基線在 y = 400 附近，字從 x = 50 開始（見 text_pdf_fixture.h）。
        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY2(changed.wait(10000), "選取沒有回報");

        const app::Selection& current = selection.selection();
        QCOMPARE(current.pageIndex, 0);
        QVERIFY(!current.isEmpty());
        QCOMPARE(current.text, QStringLiteral("Hello"));

        // quad 必須落在頁面內，且是頁面座標而不是裝置座標。
        for (const domain::QuadPoint& quad : current.quads) {
            const domain::RectF box = quad.boundingBox();
            QVERIFY(box.left >= 0.0 && box.right <= test::kTextPageWidth);
            QVERIFY(box.bottom >= 0.0 && box.top <= test::kTextPageHeight);
            QVERIFY(box.height() > 0.0);
        }
    }

    void highlightIsWrittenWithAppearanceStreamAndAppendsOnly() {
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);
        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));

        const app::Selection& current = selection.selection();
        QVERIFY(!current.isEmpty());

        app::HighlightRequest request;
        request.path = path_;
        request.pageIndex = current.pageIndex;
        request.quads = current.quads;
        request.author = QStringLiteral("測試者");
        request.contents = current.text;

        app::AnnotationService service;
        const app::HighlightResult result = service.addHighlight(request);
        QVERIFY2(result.ok, qPrintable(result.message));

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        file.close();

        // 增量儲存的定義：原檔位元組原封不動，新內容接在後面。
        // 這條不成立的話，既有數位簽章會從「有效、簽章後有變更」變成「無效」。
        QVERIFY(saved.size() > original_.size());
        QCOMPARE(saved.left(original_.size()), original_);

        engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(saved.constData(), static_cast<std::size_t>(saved.size())));

        // 物件層通道會一併建立 /Popup 子註解：Acrobat 以它決定註釋視窗的位置，
        // 沒有它時註解仍然看得到，但雙擊不會開出視窗。
        QCOMPARE(document.annotationCount(0), 2);
        QCOMPARE(document.subtypeName(0, 0).value_or(std::string{}), std::string{"Highlight"});
        // 第二則是 /Popup。這個讀取器的型別對照表不含 Popup（回傳 Unknown），
        // 所以這裡只驗「不是第二則螢光筆」；Popup 會被正確過濾這件事由
        // writtenHighlightAppearsInTheAnnotationList 驗——它斷言清單只有一項。
        QVERIFY(document.subtypeName(0, 1).value_or(std::string{}) != std::string{"Highlight"});

        const auto appearance = document.appearanceStream(0, 0);
        QVERIFY2(appearance.has_value() && !appearance->empty(),
                 "/AP 沒有寫進去——PDFium 不會自動產生，缺了它其他檢視器不保證畫得出來");

        // 註解的 /Rect 必須蓋住選取範圍，否則 Acrobat 會把它裁掉。
        const auto rect = document.annotationRect(0, 0);
        QVERIFY(rect.has_value());
        const domain::RectF selectionBox = current.quads.front().boundingBox();
        QVERIFY(rect->left <= selectionBox.left + 1.0);
        QVERIFY(rect->right >= selectionBox.right - 1.0);
    }

    void searchReportsHitsIncrementallyAndSelectsThem() {
        app::SelectionController selection;
        QSignalSpy hits(&selection, &app::SelectionController::searchHitsChanged);
        QSignalSpy finished(&selection, &app::SelectionController::searchFinished);
        selection.openDocument(path_);

        // 語料在第 0 頁出現兩次 Alioth、第 1 頁一次（見 text_pdf_fixture.h）。
        selection.search(QStringLiteral("Alioth"), 0, false, false);
        QVERIFY2(finished.wait(15000), "搜尋沒有結束");
        const int total = finished.takeFirst().at(0).toInt();
        QVERIFY2(total == 3, qPrintable(QStringLiteral("命中數為 %1，預期 3").arg(total)));
        QVERIFY(hits.count() > 0);
        QCOMPARE(selection.searchHits().size(), std::size_t{3});

        // 點結果要能跳過去並選取該處，否則搜尋面板只是個清單。
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.selectSearchHit(0);
        QVERIFY(changed.wait(10000));
        QCOMPARE(selection.selection().text, QStringLiteral("Alioth"));
    }

    void searchWithNoMatchFinishesWithZero() {
        app::SelectionController selection;
        QSignalSpy finished(&selection, &app::SelectionController::searchFinished);
        selection.openDocument(path_);
        selection.search(QStringLiteral("zzzz-not-present"), 0, false, false);
        QVERIFY(finished.wait(15000));
        QCOMPARE(finished.takeFirst().at(0).toInt(), 0);
        QVERIFY(selection.searchHits().empty());
    }

    void writtenHighlightAppearsInTheAnnotationList() {
        // 寫進去讀不出來等於沒寫。這條驗的是寫入端與列舉端對同一份檔案的認知一致。
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);
        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));

        app::HighlightRequest request;
        request.path = path_;
        request.pageIndex = 0;
        request.quads = selection.selection().quads;
        request.author = QStringLiteral("測試者");
        request.contents = QStringLiteral("重點");

        app::AnnotationService service;
        QVERIFY(service.addHighlight(request).ok);

        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy annotations(&controller, &app::DocumentController::annotationsReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        controller.requestAnnotations(0, 0);
        QVERIFY2(annotations.wait(10000), "註解列表沒有回報");

        const auto& items = controller.annotations();
        // 註解列表刻意不列 Popup——它是別則註解的附屬視窗，
        // 列進去會讓每則便利貼在清單裡出現兩次。
        QCOMPARE(items.size(), std::size_t{1});
        QCOMPARE(items.front().subtype, std::string{"Highlight"});
        QCOMPARE(items.front().author, std::string{"測試者"});
        QCOMPARE(items.front().pageIndex, 0);
        QVERIFY(items.front().rect.width() > 0.0);
    }

    void revertAppendRestoresTheOriginalBytes() {
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);
        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));

        app::HighlightRequest request;
        request.path = path_;
        request.pageIndex = 0;
        request.quads = selection.selection().quads;

        app::AnnotationService service;
        const app::HighlightResult result = service.addHighlight(request);
        QVERIFY(result.ok);

        QString message;
        QVERIFY2(service.revertAppend(path_, result.previousSize, result.boundaryGuard, &message),
                 qPrintable(message));

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray reverted = file.readAll();
        file.close();
        QCOMPARE(reverted, original_);
    }

    void revertRefusesWhenFileChangedUnderneath() {
        // 檔案被別人改過還硬截，會砍掉對方寫入的內容。寧可拒絕。
        app::HighlightRequest request;
        request.path = path_;
        request.pageIndex = 0;
        request.quads.push_back(domain::QuadPoint::fromRect(domain::RectF{20, 200, 120, 220}));

        app::AnnotationService service;
        const app::HighlightResult result = service.addHighlight(request);
        QVERIFY(result.ok);

        QString message;
        // 用一個不可能相符的守衛值模擬「檔案已被改過」。
        QVERIFY(!service.revertAppend(path_, result.previousSize, QByteArray("bogus"), &message));
        QVERIFY(!message.isEmpty());
    }

    void geometricAnnotationIsWrittenWithAppearance() {
        // 幾何註解走的是同一條物件層通道，但幾何來自拖曳而不是文字選取。
        app::AnnotationRequest request;
        request.path = path_;
        request.pageIndex = 0;

        domain::Annotation annotation;
        annotation.rect = domain::RectF{50.0, 100.0, 200.0, 180.0};
        annotation.color = domain::ColorRgb{0.85, 0.1, 0.1};
        annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Square};
        request.annotation =
            app::AnnotationService::stamped(std::move(annotation), QStringLiteral("測試者"));

        app::AnnotationService service;
        const app::HighlightResult result = service.addAnnotation(request);
        QVERIFY2(result.ok, qPrintable(result.message));

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        file.close();
        QCOMPARE(saved.left(original_.size()), original_);

        engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(saved.constData(), static_cast<std::size_t>(saved.size())));
        QCOMPARE(document.subtypeName(0, 0).value_or(std::string{}), std::string{"Square"});
        const auto appearance = document.appearanceStream(0, 0);
        QVERIFY(appearance.has_value() && !appearance->empty());
    }

    // 刪除註解與它的復原（PRD-ANN-010）。
    //
    // 兩件事必須同時成立：刪完之後那一則真的不在 /Annots 裡了，而復原之後
    // 檔案**逐位元組**回到刪除前的樣子。只驗前者的話，一個「復原時多寫了
    // 一段」的實作也會過，而那會讓既有簽章從「有效、簽章後有變更」變成無效。
    void deletingAnAnnotationIsUndoableByteForByte() {
        app::AnnotationService service;

        // 先放兩則，才驗得出刪掉的是指定的那一則而不是「清空整頁」。
        for (int i = 0; i < 2; ++i) {
            app::AnnotationRequest request;
            request.path = path_;
            request.pageIndex = 0;
            domain::Annotation annotation;
            annotation.rect = domain::RectF{50.0 + i * 10, 100.0, 200.0, 180.0};
            annotation.geometry = domain::ShapeGeometry{domain::ShapeKind::Square};
            request.annotation = app::AnnotationService::stamped(
                std::move(annotation), QStringLiteral("測試者"),
                i == 0 ? QStringLiteral("第一則") : QStringLiteral("第二則"));
            const app::HighlightResult added = service.addAnnotation(request);
            QVERIFY2(added.ok, qPrintable(added.message));
        }

        QFile before(path_);
        QVERIFY(before.open(QIODevice::ReadOnly));
        const QByteArray beforeDelete = before.readAll();
        before.close();

        engine::annotations::AnnotationDocument twoAnnots;
        QVERIFY(twoAnnots.openFromMemory(beforeDelete.constData(),
                                         static_cast<std::size_t>(beforeDelete.size())));
        // 每一則註解會連同它的 /Popup 一起掛進 /Annots，因此兩則是四項。
        QCOMPARE(twoAnnots.annotationCount(0), 4);

        const app::HighlightResult removed = service.deleteAnnotation(path_, 0, 0);
        QVERIFY2(removed.ok, qPrintable(removed.message));

        QFile after(path_);
        QVERIFY(after.open(QIODevice::ReadOnly));
        const QByteArray afterDelete = after.readAll();
        after.close();

        // 仍然是增量：刪除之前的位元組原封不動。
        QCOMPARE(afterDelete.left(beforeDelete.size()), beforeDelete);

        engine::annotations::AnnotationDocument oneAnnot;
        QVERIFY(oneAnnot.openFromMemory(afterDelete.constData(),
                                        static_cast<std::size_t>(afterDelete.size())));
        // 刪一則會少兩項：註解本身與它的 /Popup。
        QCOMPARE(oneAnnot.annotationCount(0), 2);

        // 復原：截回刪除前的長度，逐位元組相同。
        QString message;
        QVERIFY2(service.revertAppend(path_, removed.previousSize, removed.boundaryGuard, &message),
                 qPrintable(message));

        QFile restored(path_);
        QVERIFY(restored.open(QIODevice::ReadOnly));
        const QByteArray afterUndo = restored.readAll();
        restored.close();
        QCOMPARE(afterUndo, beforeDelete);
    }

    void deletingOutOfRangeFailsInsteadOfDeletingSomethingElse() {
        // 序號超出範圍時必須明確失敗。回傳成功而什麼都不做，或退而刪掉
        // 最後一則，都會讓使用者以為刪對了。
        app::AnnotationService service;
        const app::HighlightResult result = service.deleteAnnotation(path_, 0, 99);
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());
    }

    void underlineAndStrikeOutUseTheSamePath() {
        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path_);
        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY(changed.wait(10000));

        for (const auto kind : {domain::TextMarkupKind::Underline,
                                domain::TextMarkupKind::StrikeOut}) {
            domain::TextMarkupGeometry geometry;
            geometry.kind = kind;
            geometry.quads = selection.selection().quads;

            domain::Annotation annotation;
            annotation.geometry = std::move(geometry);
            annotation.opacity = 1.0;

            app::AnnotationRequest request;
            request.path = path_;
            request.pageIndex = 0;
            request.annotation =
                app::AnnotationService::stamped(std::move(annotation), QStringLiteral("測試者"));

            app::AnnotationService service;
            const app::HighlightResult result = service.addAnnotation(request);
            QVERIFY2(result.ok, qPrintable(result.message));
        }

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        file.close();

        engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(saved.constData(), static_cast<std::size_t>(saved.size())));
        QCOMPARE(document.subtypeName(0, 0).value_or(std::string{}), std::string{"Underline"});
        QCOMPARE(document.subtypeName(0, 2).value_or(std::string{}), std::string{"StrikeOut"});
    }

    void highlightWithoutSelectionIsRejected() {
        app::HighlightRequest request;
        request.path = path_;
        request.pageIndex = 0;

        app::AnnotationService service;
        const app::HighlightResult result = service.addHighlight(request);
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());
    }

    void missingFileIsReportedNotSilentlyIgnored() {
        app::HighlightRequest request;
        request.path = dir_->filePath(QStringLiteral("does-not-exist.pdf"));
        request.pageIndex = 0;
        request.quads.push_back(domain::QuadPoint::fromRect(domain::RectF{10, 10, 100, 30}));

        app::AnnotationService service;
        const app::HighlightResult result = service.addHighlight(request);
        QVERIFY(!result.ok);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
    QByteArray original_;
};

QTEST_MAIN(TestHighlightFlow)
#include "test_highlight_flow.moc"
