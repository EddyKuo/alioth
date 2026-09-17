// 顯示品質選項（PRD-VIEW-013、PRD-VIEW-016）。
//
// 這支測試守的是一個極容易靜默翻轉的對映：選單上的項目叫「平滑線條」，
// 引擎旗標叫 strokeAdjust，兩者是**反的**（平滑線條開 = strokeAdjust 關 =
// 不加 FPDF_RENDER_NO_SMOOTHPATH）。把它寫反不會有任何錯誤，畫面也照樣出圖，
// 只是細線的銳利度剛好相反——而那正是使用者開這個選項的唯一理由。
//
// 另一半是快取：顯示選項改了卻不作廢舊圖磚的話，畫面會維持舊的渲染結果，
// 使用者會以為選項沒有作用。圖磚鍵不含這些選項（見 document_controller.h），
// 所以只能靠明確清快取，而「有沒有清」在程式碼上看不出來，只能靠訊號驗。
//
// Stroke Adjust 對映到 FPDF_RENDER_NO_SMOOTHPATH 是近似而非規格等價，
// 限制寫在 engine/pdfium_engine.h；這裡驗的是對映本身有沒有接對。

#include <QtTest>

#include <QSignalSpy>

#include "app/document_controller.h"

using namespace alioth;

class TestRenderQuality : public QObject {
    Q_OBJECT

private slots:
    // 預設值就是「什麼都不做」：不灰階、線條與文字影像都平滑。
    void defaultsAreTheUnmodifiedRendering() {
        app::DocumentController controller;
        const auto& options = controller.renderOptions();
        QVERIFY(!options.grayscale);
        QVERIFY(!options.strokeAdjust);
        QVERIFY(options.smoothText);
        QVERIFY(options.smoothImages);
    }

    void smoothPathsIsTheInverseOfStrokeAdjust() {
        app::DocumentController controller;

        // 關閉平滑線條 = 啟用 Stroke Adjust 近似。
        controller.setRenderQuality(/*grayscale=*/false, /*smoothPaths=*/false,
                                    /*smoothText=*/true, /*smoothImages=*/true);
        QVERIFY2(controller.renderOptions().strokeAdjust,
                 "關掉「平滑線條」沒有啟用 strokeAdjust，細線的銳利度剛好相反");

        controller.setRenderQuality(false, true, true, true);
        QVERIFY(!controller.renderOptions().strokeAdjust);
    }

    void eachOptionReachesTheEngineOptions() {
        app::DocumentController controller;
        controller.setRenderQuality(/*grayscale=*/true, /*smoothPaths=*/false,
                                    /*smoothText=*/false, /*smoothImages=*/false);
        const auto& options = controller.renderOptions();
        QVERIFY(options.grayscale);
        QVERIFY(options.strokeAdjust);
        QVERIFY(!options.smoothText);
        QVERIFY(!options.smoothImages);

        // 其他渲染選項不可以被順手改掉：夜間模式與透明度格線各有自己的入口。
        QVERIFY(!options.nightMode);
        QVERIFY(!options.transparencyGrid);
    }

    // 改了選項要作廢舊圖磚並通知呈現層重畫；沒改就不要發訊號。
    // 後者不是潔癖：每次發訊號都會觸發一輪重新排程，而顯示品質選單
    // 每次開啟都會重設一次勾選狀態。
    void changingQualityInvalidatesTilesAndNoOpIsSilent() {
        app::DocumentController controller;
        QSignalSpy changed(&controller, &app::DocumentController::pageGeometryChanged);

        controller.setRenderQuality(true, true, true, true);
        QCOMPARE(changed.count(), 1);

        controller.setRenderQuality(true, true, true, true);
        QVERIFY2(changed.count() == 1, "選項沒有改變卻仍然作廢了快取");

        controller.setRenderQuality(true, true, true, false);
        QCOMPARE(changed.count(), 2);
    }
};

QTEST_MAIN(TestRenderQuality)
#include "test_render_quality.moc"
