// 連結面板（PRD-UI-003；連結本身的讀取見 tests/test_links.cpp）。
//
// 面板釘的是三件用讀程式碼看不出來的事：
//
//   1. 外部網址**完整**顯示。截短的網址正是釣魚連結最好用的偽裝，
//      而面板存在的理由就是讓使用者在按下去之前看到目的地。
//   2. 按下去只發訊號，不自己開啟。面板若直接呼叫 QDesktopServices，
//      PRD §8.2 的確認流程就多了一個側門，而那個側門不會有任何錯誤訊息。
//   3. 晚到的回覆不套用。快速翻頁時第 0 頁的回覆會在第 5 頁到達，
//      套上去的話面板顯示的是別頁的連結，而且看起來完全正常。

#include <QtTest>

#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "app/document_controller.h"
#include "ui/links_panel.h"

using namespace alioth;

namespace {

// 兩頁文件：第 0 頁有內部跳轉、外部網址與一個 Launch action（必須不出現）。
QByteArray makeLinkedPdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Annots [5 0 R 6 0 R 7 0 R] >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Link /Rect [50 400 200 430] "
        "/A << /S /GoTo /D [4 0 R /Fit] >> >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Link /Rect [50 300 200 330] "
        "/A << /S /URI /URI (https://example.com/a/very/long/path?token=abcdefghijklmnop) >> >>");
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

class TestLinksPanel : public QObject {
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

    void listsBothKindsAndShowsTheWholeUrl() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy ready(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        ui::LinksPanel panel(&controller);
        panel.setPage(0);
        QVERIFY2(ready.count() > 0 || ready.wait(10000), "連結沒有回報");
        panel.linksArrived(0);

        auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("linksTree"));
        QVERIFY(tree != nullptr);
        // Launch action 不得列出來——它在引擎那層就被擋掉了，面板列出一個
        // 按了沒反應的項目比不列出來更讓人困惑。
        QCOMPARE(tree->topLevelItemCount(), 2);

        bool sawFullUrl = false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const QString target = tree->topLevelItem(i)->text(1);
            if (target.startsWith(QStringLiteral("https://"))) {
                QCOMPARE(target,
                         QStringLiteral(
                             "https://example.com/a/very/long/path?token=abcdefghijklmnop"));
                QVERIFY2(!target.contains(QStringLiteral("...")), "網址被截短了");
                sawFullUrl = true;
            }
        }
        QVERIFY2(sawFullUrl, "外部網址沒有列出來");

        auto* status = panel.findChild<QLabel*>(QStringLiteral("linksStatus"));
        QVERIFY(status != nullptr);
        QVERIFY(status->text().contains(QStringLiteral("2")));
    }

    void activatingALinkOnlyEmitsAndNeverOpensItself() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy ready(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        ui::LinksPanel panel(&controller);
        QSignalSpy activated(&panel, &ui::LinksPanel::linkActivated);
        panel.setPage(0);
        QVERIFY(ready.count() > 0 || ready.wait(10000));
        panel.linksArrived(0);

        auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("linksTree"));
        QVERIFY(tree != nullptr && tree->topLevelItemCount() > 0);
        emit tree->itemActivated(tree->topLevelItem(0), 0);
        // 開啟與否是呼叫端的事（MainWindow::followLink 帶著 PRD §8.2 的確認）。
        QCOMPARE(activated.count(), 1);
    }

    void lateRepliesForAnotherPageAreIgnored() {
        app::DocumentController controller;
        QSignalSpy opened(&controller, &app::DocumentController::documentOpened);
        QSignalSpy ready(&controller, &app::DocumentController::linksReady);
        controller.openDocument(path_);
        QVERIFY(opened.wait(10000));

        ui::LinksPanel panel(&controller);
        panel.setPage(0);
        QVERIFY(ready.count() > 0 || ready.wait(10000));

        // 使用者已經翻到第 1 頁，第 0 頁的回覆才到。
        panel.setPage(1);
        panel.linksArrived(0);

        auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("linksTree"));
        QVERIFY(tree != nullptr);
        QVERIFY2(tree->topLevelItemCount() == 0, "晚到的回覆把別頁的連結套上去了");
    }

    void noDocumentSaysSoInsteadOfLookingEmpty() {
        // 空清單有兩種意思：「沒開文件」與「這頁沒有連結」。分不出來的話，
        // 使用者會以為文件裡真的沒有連結。
        app::DocumentController controller;
        ui::LinksPanel panel(&controller);
        auto* status = panel.findChild<QLabel*>(QStringLiteral("linksStatus"));
        QVERIFY(status != nullptr);
        const QString closed = status->text();
        QVERIFY(!closed.isEmpty());

        panel.setPage(-1);
        QCOMPARE(status->text(), closed);
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
};

QTEST_MAIN(TestLinksPanel)
#include "test_links_panel.moc"
