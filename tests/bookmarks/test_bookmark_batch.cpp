// 書籤批次操作的檔案層驗證（PRD-BM-005 / 006 / 007 / 008 / 018 / 019，WBS 9）。
//
// 純邏輯在 test_bookmark_ops 已經測過，這一支只驗「寫進 PDF 之後別人看不看得到」：
// 目錄頁的頁數與連結由 PDFium 讀回來，頁序重排由頁面尺寸辨識，結構由 qpdf --check
// 把關。判斷標準是零警告——qpdf 的警告典型內容是「/Length 不對，已自行修正」，
// 而那正是檢視器會幫忙修、簽章驗證不會的一類缺陷。

#include <QtTest>

#include <QTemporaryDir>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "bookmark_fixture.h"
#include "engine/bookmarks/annotation_sources.h"
#include "engine/bookmarks/bookmark_document.h"
#include "engine/pdfium_engine.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::domain::bookmarks;
using alioth::engine::bookmarks::BookmarkDocument;

namespace {

template <typename T>
class Latch {
public:
    void set(T value) {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            ready_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait(int milliseconds = 15000) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this] { return ready_; });
    }

    [[nodiscard]] const T& value() const { return value_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    T value_{};
};

Bookmark leaf(std::string title, std::int32_t page) {
    Bookmark node;
    node.title = std::move(title);
    node.target = BookmarkTarget::direct(Destination::fitPage(page));
    return node;
}

// 每一頁的 MediaBox 寬度就是它的身分證（見 bookmark_fixture.h）。
struct OpenedDocument {
    std::unique_ptr<engine::PdfiumEngine> engine;
    std::int32_t pageCount{0};
    std::vector<double> widths;
    std::vector<domain::Rotation> rotations;
};

bool openWithPdfium(const QString& path, OpenedDocument& out) {
    out.engine = std::make_unique<engine::PdfiumEngine>();
    Latch<engine::OpenResult> open;
    out.engine->openDocument(path.toStdString(), "",
                             [&open](engine::OpenResult r) { open.set(std::move(r)); });
    if (!open.wait() || !open.value().ok()) return false;
    out.pageCount = open.value().info.pageCount;

    for (std::int32_t i = 0; i < out.pageCount; ++i) {
        Latch<std::optional<domain::PageInfo>> info;
        out.engine->pageInfo(i, [&info](std::optional<domain::PageInfo> value) {
            info.set(std::move(value));
        });
        if (!info.wait() || !info.value().has_value()) return false;
        out.widths.push_back(info.value()->sizePt.width);
        out.rotations.push_back(info.value()->intrinsicRotation);
    }
    return true;
}

}  // namespace

class TestBookmarkBatch : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> dir_;

    QString writeOut(const std::string& bytes, const QString& name) {
        const QString path = dir_->filePath(name);
        if (!test::bookmarks::writeBytes(path, bytes)) return {};
        return path;
    }

    bool structureIsClean(const QString& path, const QString& label, bool& skipped) {
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        skipped = check.status == test::QpdfStatus::NotAvailable;
        if (skipped) return true;
        if (!check.clean()) {
            QTest::qFail(test::describeQpdfFailure(label, check), __FILE__, __LINE__);
            return false;
        }
        return true;
    }

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    // ------------------------------------------------------------------
    // PRD-BM-005 目錄頁、PRD-BM-008 由書籤建連結
    // ------------------------------------------------------------------

    void tocPageIsInsertedWithLinks() {
        const std::string source = test::bookmarks::makeBlankDocument(6);

        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        document.tree().push_back(leaf("Alpha", 0));
        document.tree().push_back(leaf("Beta", 3));
        document.tree().back().children.push_back(leaf("Beta detail", 4));

        const engine::bookmarks::TocBuildResult toc = document.buildTableOfContents();
        QVERIFY2(toc.ok, toc.diagnostic.c_str());
        QCOMPARE(toc.pageObjects.size(), std::size_t{1});
        QCOMPARE(toc.linkCount, std::size_t{3});
        QVERIFY(!toc.degradedNonAscii);
        QVERIFY(document.commitOutline().ok);

        const engine::objects::BuildResult built = document.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        QCOMPARE(built.bytes.compare(0, source.size(), source), 0);

        const QString path = writeOut(built.bytes, QStringLiteral("toc.pdf"));
        QVERIFY(!path.isEmpty());

        OpenedDocument opened;
        QVERIFY2(openWithPdfium(path, opened), "PDFium 開不了含目錄頁的檔案");
        QCOMPARE(opened.pageCount, 7);
        // 目錄頁插在最前面，尺寸是 A4；原本第 0 頁的寬度 100 應該退到索引 1。
        QVERIFY(opened.widths[0] > 500.0);
        QCOMPARE(opened.widths[1], 100.0);

        // 連結必須真的掛在目錄頁上，否則使用者看到的是一頁不能點的目錄。
        Latch<std::vector<domain::AnnotationSummary>> annots;
        opened.engine->pageAnnotations(0, [&annots](std::vector<domain::AnnotationSummary> a) {
            annots.set(std::move(a));
        });
        QVERIFY(annots.wait());
        QCOMPARE(annots.value().size(), std::size_t{3});

        bool skipped = false;
        QVERIFY(structureIsClean(path, QStringLiteral("目錄頁"), skipped));
        if (skipped) QSKIP(test::qpdfSkipReason().constData());
    }

    void tocPageAtBackAndMultiPage() {
        const std::string source = test::bookmarks::makeBlankDocument(3);

        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        for (int i = 0; i < 120; ++i) {
            document.tree().push_back(leaf("Entry " + std::to_string(i), i % 3));
        }

        engine::bookmarks::TocBuildOptions options;
        options.placement = engine::bookmarks::TocPlacement::Back;
        const engine::bookmarks::TocBuildResult toc = document.buildTableOfContents(options);
        QVERIFY2(toc.ok, toc.diagnostic.c_str());
        QVERIFY2(toc.pageObjects.size() >= 2, "120 個書籤應該排不進一頁");
        QCOMPARE(toc.linkCount, std::size_t{120});

        const engine::objects::BuildResult built = document.build();
        QVERIFY(built.ok);
        const QString path = writeOut(built.bytes, QStringLiteral("toc-back.pdf"));
        QVERIFY(!path.isEmpty());

        OpenedDocument opened;
        QVERIFY(openWithPdfium(path, opened));
        QCOMPARE(opened.pageCount, 3 + static_cast<std::int32_t>(toc.pageObjects.size()));
        // 放在後面時，原本的第 0 頁仍然是第 0 頁。
        QCOMPARE(opened.widths[0], 100.0);

        bool skipped = false;
        QVERIFY(structureIsClean(path, QStringLiteral("目錄頁（置後）"), skipped));
        if (skipped) QSKIP(test::qpdfSkipReason().constData());
    }

    void tocWithNonAsciiTitlesReportsDegradation() {
        // CJK 無法以標準 14 字型輸出。降級必須被回報，不能靜默輸出亂碼。
        const std::string source = test::bookmarks::makeBlankDocument(2);
        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        document.tree().push_back(leaf("\xE7\xAB\xA0\xE7\xAF\x80", 0));

        const engine::bookmarks::TocBuildResult toc = document.buildTableOfContents();
        QVERIFY(toc.ok);
        QVERIFY2(toc.degradedNonAscii, "非 ASCII 標題的降級沒有被回報");
    }

    void emptyTreeProducesNoTocContent() {
        const std::string source = test::bookmarks::makeBlankDocument(2);
        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);

        // 空樹仍然會產生一頁（只有標題列）。這是刻意的：使用者按了「建立目錄」
        // 卻什麼都沒發生比多出一頁更難理解。
        const engine::bookmarks::TocBuildResult toc = document.buildTableOfContents();
        QVERIFY(toc.ok);
        QCOMPARE(toc.pageObjects.size(), std::size_t{1});
        QCOMPARE(toc.linkCount, std::size_t{0});
    }

    // ------------------------------------------------------------------
    // PRD-BM-006 / 007 命名目標雙向轉換
    // ------------------------------------------------------------------

    void namedDestinationsRoundTripThroughFile() {
        const std::string source = test::bookmarks::makeBlankDocument(6);

        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        document.tree().push_back(leaf("Chapter One", 1));
        document.tree().push_back(leaf("Chapter Two", 4));

        const std::vector<NamedDestination> names =
            bookmarksToNamedDestinations(document.tree());
        QCOMPARE(names.size(), std::size_t{2});
        QCOMPARE(document.tree()[0].target.kind, TargetKind::Named);

        QVERIFY(document.commitNamedDestinations(names).ok);
        QVERIFY(document.commitOutline().ok);

        const engine::objects::BuildResult built = document.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        QCOMPARE(built.bytes.compare(0, source.size(), source), 0);

        // 重新開檔：命名目標要讀得回來，而且書籤仍然指向名稱。
        BookmarkDocument reopened;
        QCOMPARE(reopened.open(built.bytes), engine::objects::SourceStatus::Ok);
        QCOMPARE(reopened.namedDestinations().size(), std::size_t{2});
        QCOMPARE(reopened.tree().size(), std::size_t{2});
        QCOMPARE(reopened.tree()[0].target.kind, TargetKind::Named);

        // 反向：把命名目標解析回直接目標，頁碼必須與原本一致。
        QCOMPARE(resolveNamedTargets(reopened.tree(), reopened.namedDestinations()),
                 std::size_t{2});
        QCOMPARE(reopened.tree()[0].target.destination.pageIndex, 1);
        QCOMPARE(reopened.tree()[1].target.destination.pageIndex, 4);

        // 命名目標寫進去之後不該有任何驗證問題。
        QVERIFY(reopened.validate().empty());

        const QString path = writeOut(built.bytes, QStringLiteral("named.pdf"));
        QVERIFY(!path.isEmpty());
        OpenedDocument opened;
        QVERIFY(openWithPdfium(path, opened));
        QCOMPARE(opened.pageCount, 6);

        bool skipped = false;
        QVERIFY(structureIsClean(path, QStringLiteral("命名目標"), skipped));
        if (skipped) QSKIP(test::qpdfSkipReason().constData());
    }

    void mergingKeepsExistingNamedDestinations() {
        const std::string source = test::bookmarks::makeBlankDocument(4);
        BookmarkDocument first;
        QCOMPARE(first.open(source), engine::objects::SourceStatus::Ok);
        QVERIFY(first.commitNamedDestinations({NamedDestination{"one", Destination::fitPage(1)}})
                    .ok);
        const engine::objects::BuildResult firstBuild = first.build();
        QVERIFY(firstBuild.ok);

        BookmarkDocument second;
        QCOMPARE(second.open(firstBuild.bytes), engine::objects::SourceStatus::Ok);
        QCOMPARE(second.namedDestinations().size(), std::size_t{1});
        QVERIFY(second.commitNamedDestinations({NamedDestination{"two", Destination::fitPage(2)}})
                    .ok);
        const engine::objects::BuildResult secondBuild = second.build();
        QVERIFY(secondBuild.ok);

        BookmarkDocument third;
        QCOMPARE(third.open(secondBuild.bytes), engine::objects::SourceStatus::Ok);
        QCOMPARE(third.namedDestinations().size(), std::size_t{2});
    }

    // ------------------------------------------------------------------
    // PRD-BM-019 驗證書籤
    // ------------------------------------------------------------------

    void validationCatchesTargetsBeyondPageCount() {
        const std::string source = test::bookmarks::makeBlankDocument(4);
        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        QCOMPARE(document.pageCount(), 4);

        document.tree().push_back(leaf("fine", 0));
        document.tree().push_back(leaf("points nowhere", 50));
        document.tree().push_back(leaf("bad name", 1));
        document.tree().back().target = BookmarkTarget::named("no-such-destination");

        const std::vector<ValidationIssue> issues = document.validate();
        QCOMPARE(issues.size(), std::size_t{2});

        bool sawOutOfRange = false;
        bool sawUnresolved = false;
        for (const ValidationIssue& issue : issues) {
            if (issue.kind == IssueKind::PageOutOfRange) {
                sawOutOfRange = true;
                const BookmarkPath expected{1};
                QCOMPARE(issue.path, expected);
            }
            if (issue.kind == IssueKind::UnresolvedName) sawUnresolved = true;
        }
        QVERIFY2(sawOutOfRange, "沒有抓到指向不存在頁面的書籤");
        QVERIFY2(sawUnresolved, "沒有抓到不存在的命名目標");
    }

    void validationOnDocumentWithoutBookmarks() {
        const std::string source = test::bookmarks::makeBlankDocument(2);
        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        QVERIFY(document.tree().empty());
        QVERIFY(document.validate().empty());
        QCOMPARE(document.truncatedOnRead(), std::size_t{0});
    }

    // ------------------------------------------------------------------
    // PRD-BM-018 依書籤排序頁面
    // ------------------------------------------------------------------

    void pagesAreReorderedToMatchBookmarks() {
        const std::string source = test::bookmarks::makeBlankDocument(5);

        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        document.tree().push_back(leaf("third", 2));
        document.tree().push_back(leaf("first", 0));

        QVERIFY2(document.reorderPagesByBookmarks().empty(), "重排回報了錯誤");
        QVERIFY(document.commitOutline().ok);

        const engine::objects::BuildResult built = document.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        QCOMPARE(built.bytes.compare(0, source.size(), source), 0);

        const QString path = writeOut(built.bytes, QStringLiteral("reordered.pdf"));
        QVERIFY(!path.isEmpty());

        OpenedDocument opened;
        QVERIFY(openWithPdfium(path, opened));
        QCOMPARE(opened.pageCount, 5);
        // 原本的寬度是 100/110/120/130/140；新頁序是 2,0,1,3,4。
        const std::vector<double> expected{120.0, 100.0, 110.0, 130.0, 140.0};
        QCOMPARE(opened.widths, expected);

        bool skipped = false;
        QVERIFY(structureIsClean(path, QStringLiteral("頁序重排"), skipped));
        if (skipped) QSKIP(test::qpdfSkipReason().constData());
    }

    void reorderFlattensNestedPageTreeWithoutLosingInheritedRotation() {
        // 中間的 /Pages 節點上有 /Rotate 90。壓平頁面樹時若沒有先把繼承屬性
        // 寫死在每一頁上，旋轉就會消失——而檔案照樣打得開，只是方向錯了。
        const std::string source = test::bookmarks::makeNestedPageTreeDocument();

        BookmarkDocument before;
        QCOMPARE(before.open(source), engine::objects::SourceStatus::Ok);
        QCOMPARE(before.pageCount(), 4);

        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        for (int page = 3; page >= 0; --page) {
            document.tree().push_back(leaf("p" + std::to_string(page), page));
        }
        QVERIFY2(document.reorderPagesByBookmarks().empty(), "重排回報了錯誤");

        const engine::objects::BuildResult built = document.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = writeOut(built.bytes, QStringLiteral("nested.pdf"));
        QVERIFY(!path.isEmpty());

        OpenedDocument opened;
        QVERIFY(openWithPdfium(path, opened));
        QCOMPARE(opened.pageCount, 4);
        // 帶 /Rotate 90 的兩頁，PDFium 回報的寬度是旋轉後的值（原本的高度）。
        const std::vector<double> expectedWidths{130.0, 120.0, 807.0, 800.0};
        QCOMPARE(opened.widths, expectedWidths);
        // 原本第 0、1 頁靠繼承取得 /Rotate 90，重排後它們在索引 3、2。
        QCOMPARE(opened.rotations[3], domain::Rotation::Cw90);
        QCOMPARE(opened.rotations[2], domain::Rotation::Cw90);
        QCOMPARE(opened.rotations[0], domain::Rotation::None);

        bool skipped = false;
        QVERIFY(structureIsClean(path, QStringLiteral("壓平頁面樹"), skipped));
        if (skipped) QSKIP(test::qpdfSkipReason().constData());
    }

    void reorderIsSkippedWhenOrderIsUnchanged() {
        const std::string source = test::bookmarks::makeBlankDocument(3);
        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        for (int page = 0; page < 3; ++page) {
            document.tree().push_back(leaf("p" + std::to_string(page), page));
        }
        QVERIFY(document.reorderPagesByBookmarks().empty());
        // 順序沒變就不該有任何待寫入的物件——附加一段什麼都沒改的更新會讓
        // 「檔案有沒有被改過」的判斷失準。
        QVERIFY(!document.appender().hasPendingObjects());
    }

    // ------------------------------------------------------------------
    // PRD-BM-014 由高亮產生書籤
    // ------------------------------------------------------------------

    void highlightsBecomeBookmarksInReadingOrder() {
        const std::string source = test::bookmarks::makeHighlightedDocument();

        engine::objects::PdfSourceDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);

        const std::vector<engine::bookmarks::HighlightRecord> records =
            engine::bookmarks::collectHighlights(document);
        // 便利貼不算高亮；三則高亮全部收下，其中一則沒有 /Contents。
        QCOMPARE(records.size(), std::size_t{3});
        QCOMPARE(QString::fromStdString(records[0].source.text), QStringLiteral("upper one"));
        QCOMPARE(QString::fromStdString(records[1].source.text), QStringLiteral("lower one"));
        QCOMPARE(records[2].source.pageIndex, 1);
        QVERIFY2(records[2].needsTextLookup,
                 "沒有 /Contents 的高亮必須被標出來，不能靜默給空標題");

        // 預設不收沒有標題的那一則：一整排空白書籤比少一則更難處理。
        const BookmarkTree tree =
            bookmarksFromHighlights(engine::bookmarks::highlightSources(records));
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("upper one"));
        QCOMPARE(tree[0].target.destination.zoom, ZoomType::XYZ);
        QCOMPARE(*tree[0].target.destination.top, 720.0);

        FromHighlightsOptions grouped;
        grouped.groupByPage = true;
        const BookmarkTree byPage = bookmarksFromHighlights(
            engine::bookmarks::highlightSources(records, true), grouped);
        QCOMPARE(byPage.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(byPage[0].title), QStringLiteral("Page 1"));
        QCOMPARE(byPage[0].children.size(), std::size_t{2});
    }

    // ------------------------------------------------------------------
    // PRD-BM-009 / 010 匯出（以真實檔案讀出來的樹為輸入）
    // ------------------------------------------------------------------

    void exportsUseTheTreeReadBackFromFile() {
        const std::string source = test::bookmarks::makeBlankDocument(4);
        BookmarkDocument document;
        QCOMPARE(document.open(source), engine::objects::SourceStatus::Ok);
        document.tree().push_back(leaf("Chapter", 0));
        document.tree().back().children.push_back(leaf("Section", 2));
        QVERIFY(document.commitOutline().ok);
        const engine::objects::BuildResult built = document.build();
        QVERIFY(built.ok);

        BookmarkDocument reopened;
        QCOMPARE(reopened.open(built.bytes), engine::objects::SourceStatus::Ok);

        const QString text = QString::fromStdString(exportAsText(reopened.tree()));
        QVERIFY(text.startsWith(QStringLiteral("Chapter\t1\n")));
        QVERIFY(text.contains(QStringLiteral("    Section\t3")));

        const QString html = QString::fromStdString(exportAsHtml(reopened.tree()));
        QVERIFY(html.contains(QStringLiteral("<li>Chapter")));
        QCOMPARE(html.count(QStringLiteral("<ul>")), html.count(QStringLiteral("</ul>")));
    }
};

QTEST_APPLESS_MAIN(TestBookmarkBatch)
#include "test_bookmark_batch.moc"
