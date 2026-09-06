// 命名目標面板與自動捲動（PRD-NAV-007、PRD-NAV-008）。

#include <QtTest>

#include <QTemporaryDir>

#include "app/navigation_service.h"

using namespace alioth;
using app::AutoScroller;
using app::DestinationRow;
using app::NavigationService;
using domain::bookmarks::ZoomType;

namespace {

// 三頁文件，帶新舊兩代命名目標語法各一組，外加一個指向不存在頁面的目標。
QByteArray makeDestinationsPdf() {
    std::vector<QByteArray> objects;

    // 1 catalog：舊式 /Dests 字典 + 新式 /Names /Dests 名稱樹
    objects.push_back(
        "<< /Type /Catalog /Pages 2 0 R /Dests 6 0 R /Names << /Dests 7 0 R >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>");
    for (int i = 0; i < 3; ++i) {
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] >>");
    }

    // 6：舊式字典。指向第 1 頁（索引 0），符合頁寬。
    objects.push_back("<< /legacy_top [3 0 R /FitH 500] >>");

    // 7：名稱樹。名稱刻意不按頁序給，用來驗排序。
    //    zeroZoom 的 /XYZ 倍率是 0，代表「沿用目前倍率」——這是最容易讀錯的一項。
    objects.push_back(
        "<< /Names [ (broken) 8 0 R (page_three) 9 0 R (page_two) 10 0 R (zeroZoom) 11 0 R ] >>");
    objects.push_back("[ 99 0 R /Fit ]");
    objects.push_back("[ 5 0 R /XYZ 10 480 2.0 ]");
    objects.push_back("[ 4 0 R /Fit ]");
    objects.push_back("[ 4 0 R /XYZ 0 500 0 ]");

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

class TestNavigationService : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("dests.pdf"));
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(makeDestinationsPdf());
        file.close();
    }

    void bothDestinationSyntaxesAreRead() {
        // 舊式 catalog /Dests 與新式 /Names /Dests 都有工具只產出其中一種。
        // 只認一邊的話，面板在半數文件上是空的。
        NavigationService service;
        std::string diagnostic;
        QVERIFY2(service.load(path_, diagnostic), diagnostic.c_str());

        QCOMPARE(service.rows().size(), std::size_t{5});
        QVERIFY(service.findByName(QStringLiteral("legacy_top")) != nullptr);
        QVERIFY(service.findByName(QStringLiteral("page_two")) != nullptr);
    }

    void rowsAreSortedByPageWithBrokenOnesLast() {
        NavigationService service;
        std::string diagnostic;
        QVERIFY(service.load(path_, diagnostic));

        const auto& rows = service.rows();
        QCOMPARE(rows.front().name, QStringLiteral("legacy_top"));  // 第 1 頁
        QCOMPARE(rows.back().name, QStringLiteral("broken"));
        QVERIFY(rows.back().broken);
        for (std::size_t i = 0; i + 1 < rows.size(); ++i) {
            if (rows[i + 1].broken) break;
            QVERIFY(rows[i].pageIndex <= rows[i + 1].pageIndex);
        }
    }

    void brokenDestinationIsFlaggedNotDropped() {
        // 指向已刪除頁面的命名目標在真實文件裡很常見。靜默丟掉會讓使用者
        // 以為文件沒有那個目標；靜默跳到第一頁更糟。
        NavigationService service;
        std::string diagnostic;
        QVERIFY(service.load(path_, diagnostic));

        const DestinationRow* row = service.findByName(QStringLiteral("broken"));
        QVERIFY(row != nullptr);
        QVERIFY(row->broken);
        QVERIFY(row->describe().contains(QStringLiteral("不存在")));
    }

    void zeroZoomMeansInheritNotZeroPercent() {
        NavigationService service;
        std::string diagnostic;
        QVERIFY(service.load(path_, diagnostic));

        const DestinationRow* row = service.findByName(QStringLiteral("zeroZoom"));
        QVERIFY(row != nullptr);
        QCOMPARE(row->zoom, ZoomType::XYZ);
        QVERIFY2(row->inheritsZoom(), "倍率 0 必須解讀為沿用目前倍率");

        const DestinationRow* explicitZoom = service.findByName(QStringLiteral("page_three"));
        QVERIFY(explicitZoom != nullptr);
        QVERIFY(!explicitZoom->inheritsZoom());
        QVERIFY(explicitZoom->describe().contains(QStringLiteral("200")));
    }

    void missingZoomAlsoInherits() {
        DestinationRow row;
        row.zoom = ZoomType::XYZ;
        QVERIFY(row.inheritsZoom());
    }

    void filterIsCaseInsensitiveOnName() {
        NavigationService service;
        std::string diagnostic;
        QVERIFY(service.load(path_, diagnostic));

        QCOMPARE(service.filter(QStringLiteral("PAGE_")).size(), std::size_t{2});
        QCOMPARE(service.filter(QStringLiteral("zerozoom")).size(), std::size_t{1});
        QCOMPARE(service.filter(QString()).size(), service.rows().size());
    }

    void unreadableFileReportsReasonAndLeavesNoStaleRows() {
        NavigationService service;
        std::string diagnostic;
        QVERIFY(service.load(path_, diagnostic));
        QVERIFY(!service.rows().empty());

        QVERIFY(!service.load(dir_->filePath(QStringLiteral("nope.pdf")), diagnostic));
        QVERIFY(!diagnostic.empty());
        // 換文件失敗時舊清單必須清掉，否則面板會顯示前一份文件的目標，
        // 而使用者點下去就會跳到毫不相干的位置。
        QVERIFY(service.rows().empty());
    }

    // -----------------------------------------------------------------------
    // PRD-NAV-008 自動捲動
    // -----------------------------------------------------------------------

    void slowSpeedStillMovesOverTime() {
        // 這是本類別存在的理由：每 tick 0.32 像素，逐次取整會永遠是 0。
        AutoScroller scroller;
        scroller.setSpeed(1);
        scroller.start();

        int total = 0;
        for (int i = 0; i < 100; ++i) total += scroller.tick(16.0);

        // 12 px/s × 1.6 秒 ≈ 19 像素。
        QVERIFY2(total >= 18 && total <= 20, qPrintable(QStringLiteral("實得 %1").arg(total)));
    }

    void accumulatedFractionIsNotLost() {
        AutoScroller scroller;
        scroller.setSpeed(5);
        scroller.start();

        const double perSecond = scroller.pixelsPerSecond();
        int total = 0;
        for (int i = 0; i < 60; ++i) total += scroller.tick(16.6667);

        const double expected = perSecond;  // 60 × 16.6667 毫秒 = 1 秒
        QVERIFY2(std::abs(total - expected) <= 1.5,
                 qPrintable(QStringLiteral("預期約 %1，實得 %2").arg(expected).arg(total)));
    }

    void speedZeroMeansStopped() {
        AutoScroller scroller;
        scroller.start();
        scroller.setSpeed(0);
        QCOMPARE(scroller.pixelsPerSecond(), 0.0);
        QCOMPARE(scroller.tick(1000.0), 0);
    }

    void speedIsClampedNotRejected() {
        AutoScroller scroller;
        scroller.setSpeed(999);
        QCOMPARE(scroller.speed(), AutoScroller::kMaxSpeed);
        scroller.setSpeed(-5);
        QCOMPARE(scroller.speed(), AutoScroller::kMinSpeed);
    }

    void speedLadderIsMonotonic() {
        AutoScroller scroller;
        double previous = -1.0;
        for (int speed = AutoScroller::kMinSpeed; speed <= AutoScroller::kMaxSpeed; ++speed) {
            scroller.setSpeed(speed);
            const double value = scroller.pixelsPerSecond();
            QVERIFY2(value > previous, qPrintable(QStringLiteral("第 %1 級沒有變快").arg(speed)));
            previous = value;
        }
    }

    void reverseFlipsSignOnly() {
        AutoScroller forward;
        forward.setSpeed(6);
        forward.start();
        AutoScroller backward;
        backward.setSpeed(6);
        backward.setReversed(true);
        backward.start();

        const int a = forward.tick(100.0);
        const int b = backward.tick(100.0);
        QVERIFY(a > 0);
        QCOMPARE(b, -a);
    }

    void inactiveScrollerDoesNotMove() {
        AutoScroller scroller;
        scroller.setSpeed(10);
        QCOMPARE(scroller.tick(1000.0), 0);
        scroller.toggle();
        QVERIFY(scroller.active());
        QVERIFY(scroller.tick(100.0) > 0);
        scroller.toggle();
        QCOMPARE(scroller.tick(1000.0), 0);
    }

    void longStallDoesNotJumpTheWholeGap() {
        // 視窗失焦十秒後回來，不該一口氣捲過整份文件——使用者會完全失去位置。
        AutoScroller scroller;
        scroller.setSpeed(10);
        scroller.start();
        const int jump = scroller.tick(10000.0);
        const int quarterSecond = static_cast<int>(scroller.pixelsPerSecond() * 0.25) + 1;
        QVERIFY2(jump <= quarterSecond,
                 qPrintable(QStringLiteral("一次跳了 %1 像素").arg(jump)));
    }

    void manualScrollResetsAccumulator() {
        AutoScroller scroller;
        scroller.setSpeed(1);
        scroller.start();
        scroller.tick(10.0);  // 累積不足一像素
        scroller.resetAccumulator();
        QCOMPARE(scroller.tick(1.0), 0);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_MAIN(TestNavigationService)
#include "test_navigation_service.moc"
