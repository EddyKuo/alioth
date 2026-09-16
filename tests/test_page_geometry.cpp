// 按需載入頁面幾何。
//
// 開檔時只問前 32 頁的真實尺寸——一次問一萬頁會塞爆那條唯一的 PDFium
// 執行緒。其餘頁面必須在捲進可視區時補問；不補的話第 33 頁之後永遠是 A4
// 佔位值，而混合尺寸文件的頁面間距、捲動位置、命中座標與圖磚邊界會全部
// 偏掉。這個缺陷不會有任何錯誤訊息，畫面上只是「排得有點鬆」，所以必須
// 由測試守住，不能靠肉眼。

#include <QtTest>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/document_controller.h"

using namespace alioth;

namespace {

constexpr int kPageCount = 48;
constexpr int kFirstOversizePage = 32;  // 開檔預載範圍之外的第一頁

// oversizeFrom 之後的頁面是 A3，之前是 A4。預設把分界切在預載範圍的邊界
// 上：那正是缺陷會出現的地方，而 A4／A3 的長寬差距大到不可能被誤差掩蓋。
// 傳 kPageCount 則產生整份都是 A4 的文件。
QByteArray makePdf(int oversizeFrom = kFirstOversizePage) {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");

    QByteArray kids;
    for (int i = 0; i < kPageCount; ++i) {
        if (i > 0) kids += " ";
        kids += QByteArray::number(i + 3) + " 0 R";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                      QByteArray::number(kPageCount) + " >>");

    for (int i = 0; i < kPageCount; ++i) {
        const char* box = i < oversizeFrom ? "[0 0 595 842]" : "[0 0 842 1191]";
        objects.push_back(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox ") + box +
                          " /Resources << >> >>");
    }

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

// 等某一頁的真實尺寸到位。幾何是非同步問到的，且一次可能收到多頁，
// 所以等的是條件成立而不是固定次數的訊號。
bool waitForGeometry(app::DocumentController& controller, std::int32_t page, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!controller.pageGeometryKnown(page)) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

}  // namespace

class TestPageGeometry : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("mixed_sizes.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(makePdf());
        file.close();

        // 整份 A4 的對照文件，用來驗換文件之後不會沿用上一份的尺寸。
        a4Path_ = dir_->filePath(QStringLiteral("a4_only.pdf"));
        QFile a4(a4Path_);
        QVERIFY(a4.open(QIODevice::WriteOnly));
        a4.write(makePdf(kPageCount));
        a4.close();
    }

    // 開檔只預載前 32 頁；第 33 頁起在被排程之前仍是佔位值。
    // 這一條同時是下一條的前提：它證明了缺陷確實可觀察。
    void openLoadsOnlyTheFirstPages() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        QVERIFY(waitForGeometry(controller, 0));
        QCOMPARE(controller.pageSizePt(0), (domain::SizeF{595.0, 842.0}));
        QVERIFY(!controller.pageGeometryKnown(kFirstOversizePage));
    }

    // 排程到第 40 頁時，它與鄰近幾頁的真實尺寸必須補進來。
    void schedulingFarPageLoadsItsRealSize() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        constexpr std::int32_t kTarget = 40;
        QVERIFY(!controller.pageGeometryKnown(kTarget));

        app::PageTileRequest request;
        request.pageIndex = kTarget;
        request.visibleInPage = domain::RectI{0, 0, 400, 400};
        request.pageSize = domain::RectI{0, 0, 842, 1191};
        controller.scheduleTiles({request}, {});

        QVERIFY2(waitForGeometry(controller, kTarget),
                 "第 40 頁的真實尺寸沒有補載，版面會永久沿用 A4 佔位值");
        QCOMPARE(controller.pageSizePt(kTarget), (domain::SizeF{842.0, 1191.0}));

        // 往後預看：頁面捲進可視區之前尺寸就該到位，否則使用者會看到
        // 版面從 A4 佔位跳成 A3。
        QVERIFY2(waitForGeometry(controller, kTarget + 1), "預看範圍沒有涵蓋下一頁");
    }

    // 預看是有界的。無界的話一次排程就把整份文件的頁面全問一遍，
    // 一萬頁文件會在那條唯一的 PDFium 執行緒上排出一萬件工作，
    // 而可見圖磚就排在它們後面。
    void lookaheadIsBounded() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        constexpr std::int32_t kTarget = 33;
        app::PageTileRequest request;
        request.pageIndex = kTarget;
        request.visibleInPage = domain::RectI{0, 0, 400, 400};
        request.pageSize = domain::RectI{0, 0, 842, 1191};
        controller.scheduleTiles({request}, {});
        QVERIFY(waitForGeometry(controller, kTarget));

        // 全部工作跑完之後，離可視頁很遠的最後一頁仍不該被問到。
        for (int i = 0; i < 40; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QVERIFY2(!controller.pageGeometryKnown(kPageCount - 1),
                 "預看範圍無界，整份文件的頁面幾何都被排進佇列");
    }

    // 換文件時，前一份文件還在路上的尺寸不可以寫進新文件。頁碼同樣合法，
    // 寫進去不會有任何錯誤，只會讓新文件的某幾頁沿用舊文件的紙張尺寸。
    void reopeningDoesNotInheritStaleSizes() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        app::PageTileRequest request;
        request.pageIndex = 40;
        request.visibleInPage = domain::RectI{0, 0, 400, 400};
        request.pageSize = domain::RectI{0, 0, 842, 1191};
        controller.scheduleTiles({request}, {});
        QVERIFY(waitForGeometry(controller, 40));

        // 立刻重開成整份 A4 的文件：上一份的 pageInfo 回呼可能還在路上。
        opened.clear();
        controller.openDocument(a4Path_);
        QVERIFY(opened.wait(10000));

        // 第 40 頁在新文件裡是 A4。不論尺寸已經問到沒有，都絕不可以是 A3——
        // 那只可能來自上一份文件。
        controller.scheduleTiles({}, {});
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QVERIFY2(controller.pageSizePt(40) != (domain::SizeF{842.0, 1191.0}),
                 "重開後第 40 頁沿用了上一份文件的 A3 尺寸");

        app::PageTileRequest request2;
        request2.pageIndex = 40;
        request2.visibleInPage = domain::RectI{0, 0, 400, 400};
        request2.pageSize = domain::RectI{0, 0, 595, 842};
        controller.scheduleTiles({request2}, {});
        QVERIFY(waitForGeometry(controller, 40));
        QCOMPARE(controller.pageSizePt(40), (domain::SizeF{595.0, 842.0}));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
    QString a4Path_;
};

QTEST_MAIN(TestPageGeometry)
#include "test_page_geometry.moc"
