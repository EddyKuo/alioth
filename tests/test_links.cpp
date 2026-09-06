// 連結註解（PRD-NAV-006）。
//
// 連結有一個安全面向：PDF 是不可信任輸入，一個看起來像頁碼的連結可以指向任何地方。
// 所以這裡除了驗「連結讀得出來」，也驗引擎**不會**把 Launch action 當成可跟隨的目標——
// PRD §8.2 明令禁止執行外部程式，而拒絕的正確方式是根本不回報它。

#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/document_controller.h"

using namespace alioth;

namespace {

// 三頁文件：第 0 頁有一個內部跳轉、一個外部網址、一個 Launch action。
QByteArray makeLinkedPdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>");

    // 第 0 頁帶三個連結註解。
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Annots [6 0 R 7 0 R 8 0 R] >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>");

    // 內部跳轉到第 2 頁（物件 5）。
    objects.push_back(
        "<< /Type /Annot /Subtype /Link /Rect [50 400 200 430] "
        "/A << /S /GoTo /D [5 0 R /Fit] >> >>");
    // 外部網址。
    objects.push_back(
        "<< /Type /Annot /Subtype /Link /Rect [50 300 200 330] "
        "/A << /S /URI /URI (https://example.com/doc) >> >>");
    // Launch action：必須被忽略。
    objects.push_back(
        "<< /Type /Annot /Subtype /Link /Rect [50 200 200 230] "
        "/A << /S /Launch /F (calc.exe) >> >>");

    QByteArray pdf = "%PDF-1.7\n";
    std::vector<int> offsets;
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[static_cast<std::size_t>(i)] +
               "\nendobj\n";
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

class TestLinks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("links.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(makeLinkedPdf());
        file.close();
    }

    void internalAndExternalLinksAreRead() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy links(&controller, &app::DocumentController::linksReady);

        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        controller.requestLinks(0);
        QVERIFY2(links.count() > 0 || links.wait(10000), "連結沒有回報");

        const auto& found = controller.linksForPage(0);

        int internal = 0;
        int external = 0;
        for (const domain::LinkTarget& link : found) {
            if (link.isExternal()) {
                ++external;
                QCOMPARE(link.uri, std::string{"https://example.com/doc"});
            } else if (link.pageIndex) {
                ++internal;
                QCOMPARE(*link.pageIndex, 2);
            }
        }

        QCOMPARE(internal, 1);
        QCOMPARE(external, 1);
        // Launch action 不得出現在可跟隨的目標裡（PRD §8.2）。
        QVERIFY2(found.size() == 2,
                 qPrintable(QStringLiteral("回報了 %1 個連結，預期 2 個——"
                                           "Launch action 可能沒有被忽略")
                                .arg(found.size())));
    }

    void uriHasNoTrailingNul() {
        // PDFium 回傳的長度含結尾的 NUL。留著它，字串比較與顯示都會出錯，
        // 而症狀是「網址看起來對但開不起來」。
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy links(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));
        controller.requestLinks(0);
        QVERIFY(links.count() > 0 || links.wait(10000));

        for (const domain::LinkTarget& link : controller.linksForPage(0)) {
            if (!link.isExternal()) continue;
            QVERIFY2(link.uri.find('\0') == std::string::npos, "URI 尾端留著 NUL");
            QCOMPARE(link.uri.size(), std::string{"https://example.com/doc"}.size());
        }
    }

    void linkRectanglesAreInPageCoordinates() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy links(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));
        controller.requestLinks(0);
        QVERIFY(links.count() > 0 || links.wait(10000));

        for (const domain::LinkTarget& link : controller.linksForPage(0)) {
            // 頁面座標、原點左下：矩形要落在 MediaBox 內且高度為正。
            QVERIFY(link.rect.left >= 0.0 && link.rect.right <= 400.0);
            QVERIFY(link.rect.bottom >= 0.0 && link.rect.top <= 500.0);
            QVERIFY(link.rect.height() > 0.0);
        }
    }

    void pageWithoutLinksReportsEmpty() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy links(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        controller.requestLinks(1);
        QVERIFY(links.count() > 0 || links.wait(10000));
        QVERIFY(controller.linksForPage(1).empty());
    }

    void repeatedRequestsDoNotRefetch() {
        // 連結在頁面載入後不會變。重複請求應該用快取，否則滑鼠每動一格
        // 就往引擎佇列塞一次工作。
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy links(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        controller.requestLinks(0);
        QVERIFY(links.count() > 0 || links.wait(10000));
        const int after = links.count();

        for (int i = 0; i < 20; ++i) controller.requestLinks(0);
        QTest::qWait(200);
        QCOMPARE(links.count(), after);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_MAIN(TestLinks)
#include "test_links.moc"
