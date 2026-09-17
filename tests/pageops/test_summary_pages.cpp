// 「文件加摘要」（PRD-ANN-028）：在指定的每一頁之後插入該頁的註解摘要頁。
//
// 摘要文字曾經限 ASCII——標準 14 字型畫不出中文，而中文註解是本產品最常見的
// 使用情境之一。ADR-007 之後純文字排版走內嵌思源黑體子集，但摘要流程是
// 另一條路徑（逐段量頁數 → 合成中繼 PDF → interleavePagesFrom 插入），
// 中間任何一段自己擋掉非 ASCII 都會讓底層的支援白費。這支測試因此從
// 服務層的入口驗，而不是驗 createPdfFromPlainText。
//
// 三個驗收點：
//   - 插入位置正確（摘要頁在它所屬的那一頁之後，不是全部堆在檔尾）
//   - 中文摘要能產出且內嵌字型（字型不在時明確失敗，不是靜默掉字）
//   - 空白摘要不會插出一頁空頁

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <memory>

#include "app/page_operations_service.h"
#include "engine/fonts/cjk_font_library.h"
#include "engine/pageops/page_boxes.h"
#include "pageops_fixture.h"
#include "pageops_readback.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using alioth::test::pageops::FixturePage;

namespace {

FixturePage textPage(const std::string& text) {
    FixturePage page;
    page.media = domain::RectF{0, 0, 400, 600};
    page.text = text;
    page.textAt = domain::PointF{20.0, 500.0};
    return page;
}

QString writeFixture(const QTemporaryDir& dir, const QString& name,
                     const std::vector<FixturePage>& pages) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    const std::string bytes = alioth::test::pageops::makeFixturePdf(pages);
    file.write(bytes.data(), static_cast<qint64>(bytes.size()));
    file.close();
    return path;
}

std::string readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace

class TestSummaryPages : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void summaryPageFollowsItsOwnPage() {
        const QString path = writeFixture(*dir_, QStringLiteral("ascii.pdf"),
                                          {textPage("PageOne"), textPage("PageTwo")});
        QVERIFY(!path.isEmpty());

        app::PageOperationsService service;
        const auto result = service.insertSummaryPages(
            path, {{0, QStringLiteral("Note on page one")}},
            app::RewriteConsent::confirmed());
        QVERIFY2(result.ok, result.message.toUtf8().constData());

        const std::string bytes = readAll(path);
        QCOMPARE(alioth::test::pageops::documentPageCount(bytes), 3);
        // 摘要頁插在第一頁之後，所以原本的第二頁被推到第三頁。
        QVERIFY(alioth::test::pageops::pageText(bytes, dir_->path(), 0).find("PageOne") !=
                std::string::npos);
        QVERIFY2(alioth::test::pageops::pageText(bytes, dir_->path(), 1).find("Note on page one") !=
                     std::string::npos,
                 "摘要頁沒有插在它所屬的那一頁之後");
        QVERIFY(alioth::test::pageops::pageText(bytes, dir_->path(), 2).find("PageTwo") !=
                std::string::npos);
    }

    void chineseSummaryTextIsAccepted() {
        const QString path =
            writeFixture(*dir_, QStringLiteral("cjk.pdf"), {textPage("PageOne")});
        QVERIFY(!path.isEmpty());
        const std::string before = readAll(path);

        app::PageOperationsService service;
        const auto result = service.insertSummaryPages(
            path, {{0, QStringLiteral("審閱意見：這一段需要補充說明")}},
            app::RewriteConsent::confirmed());

        if (!engine::fonts::CjkFontLibrary::instance().available()) {
            // 字型不在時必須明確失敗。靜默丟字的摘要頁看起來是空白的，
            // 而使用者會以為那一頁本來就沒有註解。
            QVERIFY2(!result.ok, "沒有內嵌字型卻宣稱摘要成功");
            QVERIFY(!result.message.isEmpty());
            QCOMPARE(readAll(path), before);
            return;
        }

        QVERIFY2(result.ok, result.message.toUtf8().constData());
        const std::string bytes = readAll(path);
        QCOMPARE(alioth::test::pageops::documentPageCount(bytes), 2);
        QVERIFY2(bytes.find("/CJK") != std::string::npos,
                 "中文摘要沒有走內嵌子集，畫出來會是空白或替代字形");
    }

    // 全部摘要都是空白時不插任何頁：插一頁空的比什麼都不做更糟，
    // 因為使用者得自己找出來刪掉。
    void blankSummariesInsertNothing() {
        const QString path =
            writeFixture(*dir_, QStringLiteral("blank.pdf"), {textPage("PageOne")});
        const std::string before = readAll(path);

        app::PageOperationsService service;
        const auto result = service.insertSummaryPages(path, {{0, QStringLiteral("   ")}},
                                                       app::RewriteConsent::confirmed());
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());
        QCOMPARE(readAll(path), before);
    }

    // 並排（PRD-ANN-028 的第三種版面）：每一頁與它的摘要併成一張，
    // 目標頁兩倍寬、等高。原內容維持原尺寸——把正文縮成一半來騰出摘要空間，
    // 等於為了看註解而讓正文變難讀，而使用者正是為了對照才選並排的。
    void sideBySidePlacesSummaryOnTheRightHalf() {
        const QString path = writeFixture(*dir_, QStringLiteral("side.pdf"),
                                          {textPage("PageOne"), textPage("PageTwo")});
        QVERIFY(!path.isEmpty());

        app::PageOperationsService service;
        const auto result = service.insertSideBySideSummary(
            path, {{0, QStringLiteral("Note on page one")}},
            app::RewriteConsent::confirmed());
        QVERIFY2(result.ok, result.message.toUtf8().constData());

        const std::string bytes = readAll(path);
        // 頁數不變：原頁與摘要頁併成一張，不是多出一張。
        QCOMPARE(alioth::test::pageops::documentPageCount(bytes), 2);

        const auto sizes = alioth::engine::pageops::readVisiblePageSizes(bytes);
        QCOMPARE(static_cast<int>(sizes.size()), 2);
        QCOMPARE(sizes[0].width, 800.0);   // 400 × 2
        QCOMPARE(sizes[0].height, 600.0);
        // 沒有註解的頁面原樣保留，不該被併成一張右半空白的頁。
        QCOMPARE(sizes[1].width, 400.0);

        // 兩邊的內容都要在同一頁上，而且各在各的半邊。
        const std::string left = alioth::test::pageops::textInArea(
            bytes, dir_->path(), 0, domain::RectF{0, 0, 400, 600});
        const std::string right = alioth::test::pageops::textInArea(
            bytes, dir_->path(), 0, domain::RectF{400, 0, 800, 600});
        QVERIFY2(left.find("PageOne") != std::string::npos, "原頁面不在左半邊");
        QVERIFY2(right.find("Note on page one") != std::string::npos, "摘要不在右半邊");
        QVERIFY2(left.find("Note on page one") == std::string::npos,
                 "摘要跑到左半邊，兩者疊在一起");

        // 第二頁沒有註解，內容原樣。
        const std::string second = alioth::test::pageops::pageText(bytes, dir_->path(), 1);
        QVERIFY(second.find("PageTwo") != std::string::npos);

        assertQpdfClean(path);
    }

    // 多頁都有註解時，每一張並排頁都要留在它原本的位置。
    // 由後往前插入的技巧一旦寫反，症狀是「第 30 頁之後全部對不上」，
    // 兩頁的測試文件完全看不出來——所以這裡用四頁、註解分佈在頭尾。
    void sideBySideKeepsPageOrderAcrossSeveralPages() {
        const QString path =
            writeFixture(*dir_, QStringLiteral("side_many.pdf"),
                         {textPage("Alpha"), textPage("Bravo"), textPage("Charlie"),
                          textPage("Delta")});
        QVERIFY(!path.isEmpty());

        app::PageOperationsService service;
        const auto result = service.insertSideBySideSummary(
            path,
            {{0, QStringLiteral("First note")},
             {2, QStringLiteral("Third note")},
             {3, QStringLiteral("Fourth note")}},
            app::RewriteConsent::confirmed());
        QVERIFY2(result.ok, result.message.toUtf8().constData());

        const std::string bytes = readAll(path);
        QCOMPARE(alioth::test::pageops::documentPageCount(bytes), 4);

        const auto sizes = alioth::engine::pageops::readVisiblePageSizes(bytes);
        QCOMPARE(static_cast<int>(sizes.size()), 4);
        QCOMPARE(sizes[0].width, 800.0);  // 有註解 → 並排
        QCOMPARE(sizes[1].width, 400.0);  // 沒有註解 → 原樣
        QCOMPARE(sizes[2].width, 800.0);
        QCOMPARE(sizes[3].width, 800.0);

        // 每一頁的原文都還在它原本的順序上。
        const char* expected[] = {"Alpha", "Bravo", "Charlie", "Delta"};
        for (int i = 0; i < 4; ++i) {
            const std::string text = alioth::test::pageops::pageText(bytes, dir_->path(), i);
            QVERIFY2(text.find(expected[i]) != std::string::npos,
                     std::string("第 ").append(std::to_string(i))
                         .append(" 頁不是 ").append(expected[i]).c_str());
        }
        // 摘要也要各自跟對頁面，不能全部跑到同一頁去。
        QVERIFY(alioth::test::pageops::pageText(bytes, dir_->path(), 0).find("First note") !=
                std::string::npos);
        QVERIFY(alioth::test::pageops::pageText(bytes, dir_->path(), 2).find("Third note") !=
                std::string::npos);
        QVERIFY(alioth::test::pageops::pageText(bytes, dir_->path(), 3).find("Fourth note") !=
                std::string::npos);

        assertQpdfClean(path);
    }

    void sideBySideWithoutAnyCommentsLeavesTheFileAlone() {
        const QString path =
            writeFixture(*dir_, QStringLiteral("side_blank.pdf"), {textPage("Alpha")});
        const std::string before = readAll(path);

        app::PageOperationsService service;
        const auto result = service.insertSideBySideSummary(path, {{0, QStringLiteral("  ")}},
                                                            app::RewriteConsent::confirmed());
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());
        QCOMPARE(readAll(path), before);
    }

private:
    // qpdf 不存在時 QSKIP 而不是通過：CI 上缺工具卻靜默綠燈，比紅燈更糟。
    void assertQpdfClean(const QString& path) {
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        if (check.status == test::QpdfStatus::NotAvailable) {
            QSKIP("找不到 qpdf，結構檢查略過");
        }
        QVERIFY2(check.clean(), check.output.toUtf8().constData());
    }

    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestSummaryPages)
#include "test_summary_pages.moc"
