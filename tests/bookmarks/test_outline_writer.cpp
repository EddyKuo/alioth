// 書籤樹寫入（PRD-BM-001，WBS 9）。
//
// 自己寫的位元組必須由**別人**讀回來才算數：這裡用 PdfiumEngine::outline()
// （走 PDFium 的 FPDFBookmark_* API）驗層級與順序，用 qpdf --check 驗結構。
// 用自己的剖析器驗自己寫的東西只會證明我們前後一致地寫錯。

#include <QtTest>

#include <QTemporaryDir>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "bookmark_fixture.h"
#include "engine/bookmarks/bookmark_document.h"
#include "engine/bookmarks/outline_reader.h"
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

Bookmark leaf(std::string title, std::int32_t page, ZoomType zoom = ZoomType::Fit) {
    Bookmark node;
    node.title = std::move(title);
    Destination destination;
    destination.pageIndex = page;
    destination.zoom = zoom;
    node.target = BookmarkTarget::direct(destination);
    return node;
}

// PDFium 讀回來的樹攤平成「深度 + 標題 + 頁碼」，方便逐項比對。
struct FlatEntry {
    int depth{0};
    QString title;
    int pageIndex{-1};
};

void flattenOutline(const std::vector<domain::OutlineNode>& nodes, int depth,
                    std::vector<FlatEntry>& out) {
    for (const domain::OutlineNode& node : nodes) {
        out.push_back(FlatEntry{depth, QString::fromStdString(node.title),
                                node.pageIndex.has_value() ? *node.pageIndex : -1});
        flattenOutline(node.children, depth + 1, out);
    }
}

// qpdf 的 JSON 輸出裡有完整的書籤樹（title / kids / destpageforposfrom1）。
//
// 需要第三個驗證管道的理由很具體：PdfiumEngine::outline() 目前在「同層有兩個
// 以上兄弟、而且不是最後一個帶子節點」的樹上會存取已釋放的記憶體（它把
// std::vector 元素的位址存進走訪堆疊，後續 push_back 重新配置後那個位址就懸空），
// 所以分支型的樹沒辦法用它驗。qpdf 與 PDFium、與我們的剖析器都無關，
// 是這個情況下唯一有意義的第二意見。
struct JsonEntry {
    int depth{0};
    QString title;
    int pageIndex{-1};
};

void flattenJsonOutlines(const QJsonArray& array, int depth, std::vector<JsonEntry>& out) {
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        JsonEntry entry;
        entry.depth = depth;
        entry.title = object.value(QStringLiteral("title")).toString();
        const QJsonValue page = object.value(QStringLiteral("destpageposfrom1"));
        entry.pageIndex = page.isDouble() ? page.toInt() - 1 : -1;
        out.push_back(entry);
        flattenJsonOutlines(object.value(QStringLiteral("kids")).toArray(), depth + 1, out);
    }
}

// 回傳 false 代表 qpdf 不可用（測試應 skip）而不是驗證失敗。
bool qpdfOutlines(const QString& path, std::vector<JsonEntry>& out, QString& diagnostic) {
    const QString executable = alioth::test::findQpdf();
    if (executable.isEmpty()) return false;

    QProcess process;
    process.start(executable, QStringList{QStringLiteral("--json=latest"),
                                          QStringLiteral("--json-key=outlines"), path});
    if (!process.waitForStarted(15000) || !process.waitForFinished(60000)) {
        diagnostic = QStringLiteral("qpdf 沒有正常結束");
        return true;
    }
    const QByteArray output = process.readAllStandardOutput();
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(output, &error);
    if (document.isNull()) {
        diagnostic = QStringLiteral("qpdf 的 JSON 解析失敗：") + error.errorString();
        return true;
    }
    flattenJsonOutlines(document.object().value(QStringLiteral("outlines")).toArray(), 0, out);
    return true;
}

}  // namespace

class TestOutlineWriter : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> dir_;
    std::string source_;

    // 寫入 → 驗純附加 → 落檔 → 由 PDFium 讀回書籤樹。
    std::vector<FlatEntry> roundTrip(const BookmarkTree& tree, const QString& name,
                                     std::string* bytesOut = nullptr) {
        std::vector<FlatEntry> flat;

        BookmarkDocument document;
        if (document.open(source_) != engine::objects::SourceStatus::Ok) {
            QTest::qFail("開檔失敗", __FILE__, __LINE__);
            return flat;
        }
        document.tree() = tree;
        const engine::bookmarks::OutlineWriteResult written = document.commitOutline();
        if (!written.ok) {
            QTest::qFail(written.diagnostic.c_str(), __FILE__, __LINE__);
            return flat;
        }

        const engine::objects::BuildResult built = document.build();
        if (!built.ok) {
            QTest::qFail(built.diagnostic.c_str(), __FILE__, __LINE__);
            return flat;
        }
        // 純附加：原檔前綴逐位元組不變，既有簽章才不會失效。
        if (built.bytes.size() <= source_.size() ||
            built.bytes.compare(0, source_.size(), source_) != 0) {
            QTest::qFail("輸出不是原檔加附加段", __FILE__, __LINE__);
            return flat;
        }
        if (bytesOut != nullptr) *bytesOut = built.bytes;

        const QString path = dir_->filePath(name);
        if (!test::bookmarks::writeBytes(path, built.bytes)) {
            QTest::qFail("無法寫入暫存檔", __FILE__, __LINE__);
            return flat;
        }

        engine::PdfiumEngine engine;
        Latch<engine::OpenResult> open;
        engine.openDocument(path.toStdString(), "",
                            [&open](engine::OpenResult r) { open.set(std::move(r)); });
        if (!open.wait() || !open.value().ok()) {
            QTest::qFail("PDFium 開不了寫出來的檔案", __FILE__, __LINE__);
            return flat;
        }

        Latch<std::vector<domain::OutlineNode>> outline;
        engine.outline([&outline](std::vector<domain::OutlineNode> nodes) {
            outline.set(std::move(nodes));
        });
        if (!outline.wait()) {
            QTest::qFail("讀取書籤逾時", __FILE__, __LINE__);
            return flat;
        }
        flattenOutline(outline.value(), 0, flat);
        return flat;
    }

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        source_ = test::bookmarks::makeBlankDocument(12);
    }

    void hierarchyAndOrderSurvivePdfium() {
        // 這棵樹刻意讓每一層只有**最後一個**兄弟帶子節點。不是為了測試方便，
        // 而是繞開 PdfiumEngine::outline() 的懸空指標（見檔案上方 qpdfOutlines
        // 的說明）；分支型的樹由 branchingTreeVerifiedByQpdf 以 qpdf 驗。
        BookmarkTree tree;
        tree.push_back(leaf("Chapter One", 0));
        Bookmark chapter2 = leaf("Chapter Two", 4);
        chapter2.open = true;
        chapter2.children.push_back(leaf("Section 2.1", 5));
        Bookmark section22 = leaf("Section 2.2", 6);
        section22.open = true;
        section22.children.push_back(leaf("Sub 2.2.1", 7));
        chapter2.children.push_back(std::move(section22));
        tree.push_back(std::move(chapter2));

        std::string bytes;
        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("tree.pdf"), &bytes);
        QCOMPARE(flat.size(), std::size_t{5});

        const std::vector<FlatEntry> expected{
            {0, QStringLiteral("Chapter One"), 0}, {0, QStringLiteral("Chapter Two"), 4},
            {1, QStringLiteral("Section 2.1"), 5}, {1, QStringLiteral("Section 2.2"), 6},
            {2, QStringLiteral("Sub 2.2.1"), 7}};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            QCOMPARE(flat[i].depth, expected[i].depth);
            QCOMPARE(flat[i].title, expected[i].title);
            QCOMPARE(flat[i].pageIndex, expected[i].pageIndex);
        }

        // PDFium 讀得回來不代表結構是對的：它對壞掉的 xref 與懸空參照容忍度極高。
        const QString path = dir_->filePath(QStringLiteral("tree.pdf"));
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        if (check.status == test::QpdfStatus::NotAvailable) {
            QSKIP(test::qpdfSkipReason().constData());
        }
        QVERIFY2(check.clean(), test::describeQpdfFailure(QStringLiteral("書籤寫入"), check));
    }

    void branchingTreeVerifiedByQpdf() {
        // 分支型的樹：同一層有多個兄弟，而且不是最後一個帶子節點。
        // 這正是 PdfiumEngine::outline() 會踩到懸空指標的形狀，所以改由 qpdf 驗。
        BookmarkTree tree;
        Bookmark chapter1 = leaf("Chapter One", 0);
        chapter1.open = true;
        chapter1.children.push_back(leaf("Section 1.1", 1));
        chapter1.children.back().children.push_back(leaf("Sub 1.1.1", 2));
        chapter1.children.push_back(leaf("Section 1.2", 3));
        Bookmark chapter2 = leaf("Chapter Two", 4);
        chapter2.children.push_back(leaf("Section 2.1", 5));
        tree.push_back(std::move(chapter1));
        tree.push_back(std::move(chapter2));

        BookmarkDocument document;
        QCOMPARE(document.open(source_), engine::objects::SourceStatus::Ok);
        document.tree() = tree;
        const engine::bookmarks::OutlineWriteResult written = document.commitOutline();
        QVERIFY2(written.ok, written.diagnostic.c_str());
        QCOMPARE(written.itemCount, std::size_t{6});

        const engine::objects::BuildResult built = document.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());
        QCOMPARE(built.bytes.compare(0, source_.size(), source_), 0);

        const QString path = dir_->filePath(QStringLiteral("branching.pdf"));
        QVERIFY(test::bookmarks::writeBytes(path, built.bytes));

        std::vector<JsonEntry> entries;
        QString diagnostic;
        if (!qpdfOutlines(path, entries, diagnostic)) {
            QSKIP(test::qpdfSkipReason().constData());
        }
        QVERIFY2(diagnostic.isEmpty(), qPrintable(diagnostic));

        const std::vector<JsonEntry> expected{
            {0, QStringLiteral("Chapter One"), 0}, {1, QStringLiteral("Section 1.1"), 1},
            {2, QStringLiteral("Sub 1.1.1"), 2},   {1, QStringLiteral("Section 1.2"), 3},
            {0, QStringLiteral("Chapter Two"), 4}, {1, QStringLiteral("Section 2.1"), 5}};
        QCOMPARE(entries.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            QCOMPARE(entries[i].depth, expected[i].depth);
            QCOMPARE(entries[i].title, expected[i].title);
            QCOMPARE(entries[i].pageIndex, expected[i].pageIndex);
        }

        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        QVERIFY2(check.clean(), test::describeQpdfFailure(QStringLiteral("分支書籤樹"), check));
    }

    void deepNestingSurvivesRoundTrip() {
        // PRD-NAV-003 要求巢狀 ≥ 8 層。這裡做 10 層，確定不是剛好卡在門檻上。
        BookmarkTree tree;
        tree.push_back(leaf("L0", 0));
        Bookmark* cursor = &tree.back();
        for (int i = 1; i < 10; ++i) {
            cursor->open = true;
            cursor->children.push_back(leaf("L" + std::to_string(i), i));
            cursor = &cursor->children.back();
        }

        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("deep.pdf"));
        QCOMPARE(flat.size(), std::size_t{10});
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(flat[static_cast<std::size_t>(i)].depth, i);
            QCOMPARE(flat[static_cast<std::size_t>(i)].title,
                     QStringLiteral("L%1").arg(i));
            QCOMPARE(flat[static_cast<std::size_t>(i)].pageIndex, i);
        }
    }

    void emptyTreeClearsBookmarks() {
        const std::vector<FlatEntry> flat = roundTrip(BookmarkTree{}, QStringLiteral("empty.pdf"));
        QVERIFY(flat.empty());
    }

    void singleBookmark() {
        BookmarkTree tree;
        tree.push_back(leaf("Only", 11));
        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("single.pdf"));
        QCOMPARE(flat.size(), std::size_t{1});
        QCOMPARE(flat[0].pageIndex, 11);
    }

    void nonAsciiTitlesSurvive() {
        BookmarkTree tree;
        // 標題是 UTF-16BE 十六進位字串寫出去的；逐位元組跳脫的實作會在這裡壞掉。
        tree.push_back(leaf("\xE7\xAB\xA0\xE7\xAF\x80\xE4\xB8\x80", 0));
        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("cjk.pdf"));
        QCOMPARE(flat.size(), std::size_t{1});
        QCOMPARE(flat[0].title, QString::fromUtf8("\xE7\xAB\xA0\xE7\xAF\x80\xE4\xB8\x80"));
    }

    void treeEditsThenWrite() {
        // 建立 → 改名 → 移動 → 刪除，全部在領域層做完再寫一次。
        BookmarkTree tree;
        tree.push_back(leaf("A", 0));
        tree.push_back(leaf("B", 1));
        tree.push_back(leaf("C", 2));
        QVERIFY(renameNode(tree, BookmarkPath{1}, "B renamed"));
        QVERIFY(moveNode(tree, BookmarkPath{2}, BookmarkPath{0}, 0));
        QVERIFY(removeNode(tree, BookmarkPath{1}));

        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("edited.pdf"));
        QCOMPARE(flat.size(), std::size_t{2});
        QCOMPARE(flat[0].title, QStringLiteral("A"));
        QCOMPARE(flat[0].depth, 0);
        QCOMPARE(flat[1].title, QStringLiteral("C"));
        QCOMPARE(flat[1].depth, 1);
    }

    void goToActionFormIsWrittenAndReadBack() {
        // /A << /S /GoTo /D ... >> 與 /Dest 語意相同，但 PDFium 的
        // FPDFBookmark_GetDest **不看 /A**，因此頁碼要靠我們自己的讀取端驗。
        BookmarkTree tree;
        Bookmark node = leaf("Action form", 6);
        node.target.encoding = TargetEncoding::GoToAction;
        tree.push_back(std::move(node));

        std::string bytes;
        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("action.pdf"), &bytes);
        QCOMPARE(flat.size(), std::size_t{1});
        QCOMPARE(flat[0].title, QStringLiteral("Action form"));

        BookmarkDocument reopened;
        QCOMPARE(reopened.open(bytes), engine::objects::SourceStatus::Ok);
        QCOMPARE(reopened.tree().size(), std::size_t{1});
        QCOMPARE(reopened.tree()[0].target.kind, TargetKind::Direct);
        QCOMPARE(reopened.tree()[0].target.encoding, TargetEncoding::GoToAction);
        QCOMPARE(reopened.tree()[0].target.destination.pageIndex, 6);
    }

    void allZoomTypesRoundTrip() {
        BookmarkTree tree;
        const auto add = [&tree](const char* title, Destination destination) {
            Bookmark node;
            node.title = title;
            node.target = BookmarkTarget::direct(destination);
            tree.push_back(std::move(node));
        };

        Destination xyz;
        xyz.pageIndex = 0;
        xyz.zoom = ZoomType::XYZ;
        xyz.left = 12.5;
        xyz.top = 700.0;
        xyz.zoomFactor = 1.5;
        add("xyz", xyz);

        add("fit", Destination::fitPage(1));

        Destination fitH;
        fitH.pageIndex = 2;
        fitH.zoom = ZoomType::FitH;
        fitH.top = 640.0;
        add("fith", fitH);

        Destination fitV;
        fitV.pageIndex = 3;
        fitV.zoom = ZoomType::FitV;
        fitV.left = 20.0;
        add("fitv", fitV);

        Destination fitR;
        fitR.pageIndex = 4;
        fitR.zoom = ZoomType::FitR;
        fitR.left = 10.0;
        fitR.bottom = 20.0;
        fitR.right = 90.0;
        fitR.top = 300.0;
        add("fitr", fitR);

        Destination fitB;
        fitB.pageIndex = 5;
        fitB.zoom = ZoomType::FitB;
        add("fitb", fitB);

        Destination fitBH;
        fitBH.pageIndex = 6;
        fitBH.zoom = ZoomType::FitBH;
        fitBH.top = 500.0;
        add("fitbh", fitBH);

        Destination fitBV;
        fitBV.pageIndex = 7;
        fitBV.zoom = ZoomType::FitBV;
        fitBV.left = 5.0;
        add("fitbv", fitBV);

        std::string bytes;
        const std::vector<FlatEntry> flat = roundTrip(tree, QStringLiteral("zoom.pdf"), &bytes);
        QCOMPARE(flat.size(), std::size_t{8});
        for (int i = 0; i < 8; ++i) QCOMPARE(flat[static_cast<std::size_t>(i)].pageIndex, i);

        BookmarkDocument reopened;
        QCOMPARE(reopened.open(bytes), engine::objects::SourceStatus::Ok);
        const BookmarkTree& read = reopened.tree();
        QCOMPARE(read.size(), std::size_t{8});
        QCOMPARE(read[0].target.destination.zoom, ZoomType::XYZ);
        QCOMPARE(*read[0].target.destination.left, 12.5);
        QCOMPARE(*read[0].target.destination.top, 700.0);
        QCOMPARE(*read[0].target.destination.zoomFactor, 1.5);
        QCOMPARE(read[1].target.destination.zoom, ZoomType::Fit);
        QCOMPARE(read[2].target.destination.zoom, ZoomType::FitH);
        QCOMPARE(*read[2].target.destination.top, 640.0);
        QCOMPARE(read[3].target.destination.zoom, ZoomType::FitV);
        QCOMPARE(read[4].target.destination.zoom, ZoomType::FitR);
        QCOMPARE(*read[4].target.destination.right, 90.0);
        QCOMPARE(read[5].target.destination.zoom, ZoomType::FitB);
        QCOMPARE(read[6].target.destination.zoom, ZoomType::FitBH);
        QCOMPARE(read[7].target.destination.zoom, ZoomType::FitBV);
        QCOMPARE(*read[7].target.destination.left, 5.0);
    }

    void stylesAndOpenStateSurvive() {
        BookmarkTree tree;
        Bookmark node = leaf("Styled", 0);
        node.bold = true;
        node.italic = true;
        node.hasColor = true;
        node.colorR = 1.0;
        node.colorG = 0.5;
        node.colorB = 0.0;
        node.open = true;
        node.children.push_back(leaf("child", 1));
        tree.push_back(std::move(node));

        std::string bytes;
        (void)roundTrip(tree, QStringLiteral("styled.pdf"), &bytes);

        BookmarkDocument reopened;
        QCOMPARE(reopened.open(bytes), engine::objects::SourceStatus::Ok);
        const Bookmark& read = reopened.tree()[0];
        QVERIFY(read.bold);
        QVERIFY(read.italic);
        QVERIFY(read.hasColor);
        QCOMPARE(read.colorG, 0.5);
        QVERIFY(read.open);
    }

    void targetsBeyondPageCountAreDropped() {
        BookmarkTree tree;
        tree.push_back(leaf("valid", 0));
        tree.push_back(leaf("gone", 999));

        BookmarkDocument document;
        QCOMPARE(document.open(source_), engine::objects::SourceStatus::Ok);
        document.tree() = tree;
        const engine::bookmarks::OutlineWriteResult written = document.commitOutline();
        QVERIFY(written.ok);
        // 靜默夾成最後一頁會讓使用者以為書籤是好的；必須明確回報被丟掉的目標。
        QCOMPARE(written.droppedTargets, std::size_t{1});
        QCOMPARE(written.itemCount, std::size_t{2});
    }

    void rewritingTwiceKeepsOriginalPrefix() {
        // 連續兩次批次操作也必須是純附加，否則第二次會毀掉第一次之後的簽章。
        BookmarkTree tree;
        tree.push_back(leaf("first pass", 0));

        BookmarkDocument first;
        QCOMPARE(first.open(source_), engine::objects::SourceStatus::Ok);
        first.tree() = tree;
        QVERIFY(first.commitOutline().ok);
        const engine::objects::BuildResult firstBuild = first.build();
        QVERIFY(firstBuild.ok);

        BookmarkDocument second;
        QCOMPARE(second.open(firstBuild.bytes), engine::objects::SourceStatus::Ok);
        QCOMPARE(second.tree().size(), std::size_t{1});
        second.tree()[0].title = "second pass";
        QVERIFY(second.commitOutline().ok);
        const engine::objects::BuildResult secondBuild = second.build();
        QVERIFY(secondBuild.ok);

        QCOMPARE(secondBuild.bytes.compare(0, firstBuild.bytes.size(), firstBuild.bytes), 0);

        const QString path = dir_->filePath(QStringLiteral("twice.pdf"));
        QVERIFY(test::bookmarks::writeBytes(path, secondBuild.bytes));
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        if (check.status == test::QpdfStatus::NotAvailable) {
            QSKIP(test::qpdfSkipReason().constData());
        }
        QVERIFY2(check.clean(), test::describeQpdfFailure(QStringLiteral("兩次附加"), check));
    }

    void malformedOutlineChainIsTruncatedNotHung() {
        // /Next 指回自己：讀取端必須截斷並回報，而不是無限迴圈。
        std::string pdf = test::bookmarks::makeBlankDocument(2);
        // 直接在檔尾附加一段手寫的更新，讓 catalog 指到一個自我循環的書籤。
        const std::size_t base = pdf.size();
        std::string append;
        const std::size_t outlinesOffset = base;
        append += "20 0 obj\n<< /Type /Outlines /First 21 0 R /Last 21 0 R /Count 1 >>\nendobj\n";
        const std::size_t itemOffset = base + append.size();
        append += "21 0 obj\n<< /Title (loop) /Parent 20 0 R /Next 21 0 R >>\nendobj\n";
        const std::size_t catalogOffset = base + append.size();
        append += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 20 0 R >>\nendobj\n";

        const std::size_t xref = base + append.size();
        const auto pad = [](std::size_t value) {
            const std::string digits = std::to_string(value);
            return std::string(10 - digits.size(), '0') + digits;
        };
        append += "xref\n0 1\n0000000000 65535 f \n";
        append += "1 1\n" + pad(catalogOffset) + " 00000 n \n";
        append += "20 2\n" + pad(outlinesOffset) + " 00000 n \n" + pad(itemOffset) + " 00000 n \n";
        append += "trailer\n<< /Size 22 /Root 1 0 R /Prev " +
                  std::to_string(pdf.rfind("xref\n0 ")) + " >>\nstartxref\n" +
                  std::to_string(xref) + "\n%%EOF\n";
        pdf += append;

        const engine::bookmarks::OutlineReadResult read = [&pdf] {
            engine::objects::PdfSourceDocument source;
            [[maybe_unused]] const engine::objects::SourceStatus status = source.open(pdf);
            return engine::bookmarks::readOutline(source);
        }();
        QVERIFY(read.ok);
        QCOMPARE(read.itemCount, std::size_t{1});
        QVERIFY2(read.truncated > 0, "迴圈沒有被回報");
    }
};

QTEST_APPLESS_MAIN(TestOutlineWriter)
#include "test_outline_writer.moc"
