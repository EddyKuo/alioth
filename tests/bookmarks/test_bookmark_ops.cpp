// 書籤批次操作的純邏輯（PRD-BM-002 ~ 019）。
//
// 這一支刻意不開任何 PDF：批次操作的錯誤幾乎都出在語意而不是 PDF 語法，
// 而語意錯誤在「產出的檔案打得開」這件事上完全看不出來。每個操作各驗
// 正常情形與三種邊界：空樹、單一節點、深層巢狀。

#include <QtTest>

#include <string>
#include <vector>

#include "domain/bookmark_ops.h"

using namespace alioth::domain;
using namespace alioth::domain::bookmarks;

namespace {

Bookmark leaf(std::string title, std::int32_t page) {
    Bookmark node;
    node.title = std::move(title);
    node.target = BookmarkTarget::direct(Destination::fitPage(page));
    return node;
}

// 三層、每層兩個子節點的樣本樹。
BookmarkTree sampleTree() {
    BookmarkTree tree;
    Bookmark chapter1 = leaf("Chapter One", 0);
    chapter1.children.push_back(leaf("Section 1.1", 1));
    chapter1.children.back().children.push_back(leaf("Sub 1.1.1", 2));
    chapter1.children.push_back(leaf("Section 1.2", 3));
    Bookmark chapter2 = leaf("Chapter Two", 4);
    chapter2.children.push_back(leaf("Section 2.1", 5));
    tree.push_back(std::move(chapter1));
    tree.push_back(std::move(chapter2));
    return tree;
}

// n 層單鏈，用來驗深巢狀（PRD-NAV-003 要求 ≥ 8 層）。
BookmarkTree deepChain(int depth) {
    BookmarkTree tree;
    tree.push_back(leaf("L0", 0));
    Bookmark* cursor = &tree.back();
    for (int i = 1; i < depth; ++i) {
        cursor->children.push_back(leaf("L" + std::to_string(i), i));
        cursor = &cursor->children.back();
    }
    return tree;
}

std::vector<std::string> titlesInOrder(const BookmarkTree& tree) {
    std::vector<std::string> titles;
    visit(tree, [&titles](const Bookmark& node, const BookmarkPath&, int) {
        titles.push_back(node.title);
    });
    return titles;
}

}  // namespace

class TestBookmarkOps : public QObject {
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------------
    // 樹的走訪與編輯（PRD-BM-001）
    // -----------------------------------------------------------------------

    void traversalIsPreOrder() {
        const std::vector<std::string> titles = titlesInOrder(sampleTree());
        const std::vector<std::string> expected{"Chapter One", "Section 1.1", "Sub 1.1.1",
                                                "Section 1.2", "Chapter Two", "Section 2.1"};
        QCOMPARE(titles, expected);
        QCOMPARE(nodeCount(sampleTree()), std::size_t{6});
        QCOMPARE(treeDepth(sampleTree()), 3);
        QCOMPARE(treeDepth(BookmarkTree{}), 0);
    }

    void deepNestingIsSupported() {
        const BookmarkTree tree = deepChain(12);
        QCOMPARE(treeDepth(tree), 12);
        QCOMPARE(nodeCount(tree), std::size_t{12});
        const BookmarkPath eighth{0, 0, 0, 0, 0, 0, 0, 0};  // 第 8 層
        const Bookmark* node = nodeAt(tree, eighth);
        QVERIFY(node != nullptr);
        QCOMPARE(QString::fromStdString(node->title), QStringLiteral("L7"));
    }

    void insertRemoveRename() {
        BookmarkTree tree = sampleTree();
        QVERIFY(insertNode(tree, BookmarkPath{}, 1, leaf("Inserted", 9)));
        QCOMPARE(QString::fromStdString(tree[1].title), QStringLiteral("Inserted"));

        // 索引超過容器大小要失敗而不是夾住。
        QVERIFY(!insertNode(tree, BookmarkPath{}, 99, leaf("Nope", 0)));
        QVERIFY(!insertNode(tree, BookmarkPath{42}, 0, leaf("Nope", 0)));

        Bookmark removed;
        QVERIFY(removeNode(tree, BookmarkPath{1}, &removed));
        QCOMPARE(QString::fromStdString(removed.title), QStringLiteral("Inserted"));
        QCOMPARE(nodeCount(tree), std::size_t{6});

        QVERIFY(renameNode(tree, BookmarkPath{0, 0}, "Renamed"));
        QCOMPARE(QString::fromStdString(tree[0].children[0].title), QStringLiteral("Renamed"));
        QVERIFY(!renameNode(tree, BookmarkPath{5}, "x"));

        // 刪除子樹會連同子節點一起走。
        QVERIFY(removeNode(tree, BookmarkPath{0, 0}));
        QCOMPARE(nodeCount(tree), std::size_t{4});
    }

    void moveAcrossLevels() {
        BookmarkTree tree = sampleTree();
        // 把 Section 2.1 搬到 Chapter One 底下的第一個位置。
        QVERIFY(moveNode(tree, BookmarkPath{1, 0}, BookmarkPath{0}, 0));
        QCOMPARE(QString::fromStdString(tree[0].children[0].title), QStringLiteral("Section 2.1"));
        QCOMPARE(tree[1].children.size(), std::size_t{0});
        QCOMPARE(nodeCount(tree), std::size_t{6});
    }

    void moveWithinSameLevelHandlesIndexShift() {
        BookmarkTree tree;
        for (int i = 0; i < 4; ++i) tree.push_back(leaf("N" + std::to_string(i), i));
        // 把第 0 個搬到索引 2：使用者看到的是「插在 N1 與 N2 之間」。
        QVERIFY(moveNode(tree, BookmarkPath{0}, BookmarkPath{}, 2));
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("N1"));
        QCOMPARE(QString::fromStdString(tree[1].title), QStringLiteral("N0"));
        QCOMPARE(QString::fromStdString(tree[2].title), QStringLiteral("N2"));
    }

    void moveIntoOwnDescendantIsRejected() {
        BookmarkTree tree = sampleTree();
        const std::size_t before = nodeCount(tree);
        QVERIFY(!moveNode(tree, BookmarkPath{0}, BookmarkPath{0, 0}, 0));
        // 失敗的移動不得吃掉節點。
        QCOMPARE(nodeCount(tree), before);
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Chapter One"));
    }

    void promoteAndDemote() {
        BookmarkTree tree = sampleTree();
        // Chapter Two 降級成 Chapter One 的最後一個子節點。
        QVERIFY(demoteNode(tree, BookmarkPath{1}));
        QCOMPARE(tree.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(tree[0].children.back().title),
                 QStringLiteral("Chapter Two"));

        // 再升回去，應該回到根層。
        QVERIFY(promoteNode(tree, BookmarkPath{0, 2}));
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[1].title), QStringLiteral("Chapter Two"));

        QVERIFY(!demoteNode(tree, BookmarkPath{0}));   // 沒有前一個兄弟
        QVERIFY(!promoteNode(tree, BookmarkPath{0}));  // 已在根層
    }

    void siblingMoveBoundaries() {
        BookmarkTree tree = sampleTree();
        QVERIFY(!moveSibling(tree, BookmarkPath{0}, -1));
        QVERIFY(!moveSibling(tree, BookmarkPath{1}, 1));
        QVERIFY(moveSibling(tree, BookmarkPath{0}, 1));
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Chapter Two"));
    }

    // -----------------------------------------------------------------------
    // PRD-BM-002 標題加文字
    // -----------------------------------------------------------------------

    void affixAppliesToWholeTree() {
        BookmarkTree tree = sampleTree();
        AffixOptions options;
        options.prefix = "[";
        options.suffix = "]";
        QCOMPARE(applyAffix(tree, options), std::size_t{6});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("[Chapter One]"));
        QCOMPARE(QString::fromStdString(tree[0].children[0].children[0].title),
                 QStringLiteral("[Sub 1.1.1]"));
    }

    void affixRespectsLevelAndFilter() {
        BookmarkTree tree = sampleTree();
        AffixOptions byLevel;
        byLevel.prefix = "> ";
        byLevel.level = 1;
        QCOMPARE(applyAffix(tree, byLevel), std::size_t{3});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Chapter One"));
        QCOMPARE(QString::fromStdString(tree[0].children[0].title), QStringLiteral("> Section 1.1"));

        BookmarkTree filtered = sampleTree();
        AffixOptions byText;
        byText.suffix = "!";
        byText.containing = "chapter";
        byText.caseSensitive = false;
        QCOMPARE(applyAffix(filtered, byText), std::size_t{2});

        // 沒有前綴也沒有後綴時什麼都不該發生。
        BookmarkTree untouched = sampleTree();
        QCOMPARE(applyAffix(untouched, AffixOptions{}), std::size_t{0});
        BookmarkTree empty;
        QCOMPARE(applyAffix(empty, byText), std::size_t{0});
    }

    void affixSkipsEmptyTitles() {
        BookmarkTree tree;
        tree.push_back(Bookmark{});
        AffixOptions options;
        options.prefix = "X";
        QCOMPARE(applyAffix(tree, options), std::size_t{0});
        options.skipEmptyTitles = false;
        QCOMPARE(applyAffix(tree, options), std::size_t{1});
    }

    // -----------------------------------------------------------------------
    // PRD-BM-003 每 N 頁加書籤
    // -----------------------------------------------------------------------

    void everyNPages() {
        EveryNPagesOptions options;
        options.pageCount = 10;
        options.interval = 3;
        const BookmarkTree tree = generateEveryNPages(options);
        QCOMPARE(tree.size(), std::size_t{4});  // 0, 3, 6, 9
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Page 1"));
        QCOMPARE(QString::fromStdString(tree[3].title), QStringLiteral("Page 10"));
        QCOMPARE(tree[3].target.destination.pageIndex, 9);
        QCOMPARE(treeDepth(tree), 1);
    }

    void everyNPagesEdgeCases() {
        EveryNPagesOptions zeroPages;
        QVERIFY(generateEveryNPages(zeroPages).empty());

        EveryNPagesOptions zeroInterval;
        zeroInterval.pageCount = 5;
        zeroInterval.interval = 0;
        // interval 0 會產生無限多書籤，必須是空結果而不是掛住。
        QVERIFY(generateEveryNPages(zeroInterval).empty());

        EveryNPagesOptions single;
        single.pageCount = 1;
        single.interval = 5;
        QCOMPARE(generateEveryNPages(single).size(), std::size_t{1});

        EveryNPagesOptions offset;
        offset.pageCount = 6;
        offset.interval = 2;
        offset.firstPage = 3;
        offset.titlePattern = "#{index} @ {page}";
        const BookmarkTree tree = generateEveryNPages(offset);
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("#1 @ 4"));
    }

    // -----------------------------------------------------------------------
    // PRD-BM-004 大小寫轉換
    // -----------------------------------------------------------------------

    void caseConversion() {
        QCOMPARE(QString::fromStdString(convertCase("hello world", CaseMode::Upper)),
                 QStringLiteral("HELLO WORLD"));
        QCOMPARE(QString::fromStdString(convertCase("HELLO World", CaseMode::Lower)),
                 QStringLiteral("hello world"));
        QCOMPARE(QString::fromStdString(convertCase("hello wide WORLD", CaseMode::TitleCase)),
                 QStringLiteral("Hello Wide World"));
        QCOMPARE(QString::fromStdString(convertCase("hello WORLD again", CaseMode::SentenceCase)),
                 QStringLiteral("Hello world again"));
        // 撇號不該開啟新字。
        QCOMPARE(QString::fromStdString(convertCase("don't stop", CaseMode::TitleCase)),
                 QStringLiteral("Don't Stop"));
        // 非 ASCII 位元組原樣保留，不得被逐位元組轉換成亂碼。
        const std::string cjk = "\xE7\xAB\xA0\xE7\xAF\x80 one";
        QCOMPARE(QString::fromStdString(convertCase(cjk, CaseMode::Upper)),
                 QString::fromStdString("\xE7\xAB\xA0\xE7\xAF\x80 ONE"));
    }

    void applyCaseCountsOnlyRealChanges() {
        BookmarkTree tree;
        tree.push_back(leaf("ABC", 0));
        tree.push_back(leaf("abc", 0));
        QCOMPARE(applyCase(tree, CaseMode::Upper), std::size_t{1});
        BookmarkTree empty;
        QCOMPARE(applyCase(empty, CaseMode::Lower), std::size_t{0});
    }

    // -----------------------------------------------------------------------
    // PRD-BM-011 尋找取代
    // -----------------------------------------------------------------------

    void findReplace() {
        BookmarkTree tree = sampleTree();
        FindReplaceOptions options;
        options.find = "Section";
        options.replace = "Sec.";
        QCOMPARE(findReplaceTitles(tree, options), std::size_t{3});
        QCOMPARE(QString::fromStdString(tree[0].children[0].title), QStringLiteral("Sec. 1.1"));

        BookmarkTree caseTree;
        caseTree.push_back(leaf("Alpha alpha ALPHA", 0));
        FindReplaceOptions insensitive;
        insensitive.find = "alpha";
        insensitive.replace = "X";
        insensitive.caseSensitive = false;
        QCOMPARE(findReplaceTitles(caseTree, insensitive), std::size_t{1});
        QCOMPARE(QString::fromStdString(caseTree[0].title), QStringLiteral("X X X"));

        BookmarkTree wordTree;
        wordTree.push_back(leaf("cat category cat", 0));
        FindReplaceOptions wholeWord;
        wholeWord.find = "cat";
        wholeWord.replace = "dog";
        wholeWord.wholeWord = true;
        QCOMPARE(findReplaceTitles(wordTree, wholeWord), std::size_t{1});
        QCOMPARE(QString::fromStdString(wordTree[0].title), QStringLiteral("dog category dog"));

        BookmarkTree firstOnly;
        firstOnly.push_back(leaf("a a a", 0));
        FindReplaceOptions once;
        once.find = "a";
        once.replace = "b";
        once.firstOccurrenceOnly = true;
        QCOMPARE(findReplaceTitles(firstOnly, once), std::size_t{1});
        QCOMPARE(QString::fromStdString(firstOnly[0].title), QStringLiteral("b a a"));

        // 空的搜尋字串不得無限迴圈或改動任何東西。
        BookmarkTree untouched = sampleTree();
        QCOMPARE(findReplaceTitles(untouched, FindReplaceOptions{}), std::size_t{0});
    }

    // -----------------------------------------------------------------------
    // PRD-BM-016 合併重複、PRD-BM-017 移除動作
    // -----------------------------------------------------------------------

    void mergeDuplicateSiblings() {
        BookmarkTree tree;
        tree.push_back(leaf("Same", 3));
        tree.back().children.push_back(leaf("A", 4));
        tree.push_back(leaf("same", 3));
        tree.back().children.push_back(leaf("B", 5));
        tree.push_back(leaf("Same", 9));  // 目標不同，預設不合併

        MergeDuplicatesOptions options;
        QCOMPARE(mergeDuplicates(tree, options), std::size_t{1});
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(tree[0].children.size(), std::size_t{2});

        // 放寬到不管目標，剩下的兩個也要合併。
        MergeDuplicatesOptions loose;
        loose.requireSameTarget = false;
        QCOMPARE(mergeDuplicates(tree, loose), std::size_t{1});
        QCOMPARE(tree.size(), std::size_t{1});

        BookmarkTree empty;
        QCOMPARE(mergeDuplicates(empty), std::size_t{0});
        BookmarkTree single = deepChain(1);
        QCOMPARE(mergeDuplicates(single), std::size_t{0});
    }

    void mergeKeepsTargetFromDuplicate() {
        BookmarkTree tree;
        Bookmark titleOnly;
        titleOnly.title = "Chapter";
        tree.push_back(titleOnly);
        tree.push_back(leaf("Chapter", 7));
        MergeDuplicatesOptions options;
        options.requireSameTarget = false;
        QCOMPARE(mergeDuplicates(tree, options), std::size_t{1});
        QCOMPARE(tree[0].target.kind, TargetKind::Direct);
        QCOMPARE(tree[0].target.destination.pageIndex, 7);
    }

    void removeActionsKeepsStructure() {
        BookmarkTree tree = sampleTree();
        QCOMPARE(removeActions(tree), std::size_t{6});
        QCOMPARE(nodeCount(tree), std::size_t{6});
        visit(tree, [](const Bookmark& node, const BookmarkPath&, int) {
            QVERIFY(node.target.empty());
        });
        // 第二次呼叫沒有東西可清。
        QCOMPARE(removeActions(tree), std::size_t{0});
    }

    // -----------------------------------------------------------------------
    // PRD-BM-006 / 007 命名目標雙向轉換
    // -----------------------------------------------------------------------

    void bookmarksToNamesAndBack() {
        BookmarkTree tree = sampleTree();
        const std::vector<NamedDestination> names = bookmarksToNamedDestinations(tree);
        QCOMPARE(names.size(), std::size_t{6});
        QCOMPARE(QString::fromStdString(names[0].name), QStringLiteral("BM_Chapter_One"));
        QCOMPARE(tree[0].target.kind, TargetKind::Named);

        // 反向解析回直接目標，頁碼要一致。
        QCOMPARE(resolveNamedTargets(tree, names), std::size_t{6});
        QCOMPARE(tree[0].target.kind, TargetKind::Direct);
        QCOMPARE(tree[0].target.destination.pageIndex, 0);
        QCOMPARE(tree[1].target.destination.pageIndex, 4);
    }

    void duplicateTitlesGetUniqueNames() {
        BookmarkTree tree;
        tree.push_back(leaf("Intro", 0));
        tree.push_back(leaf("Intro", 1));
        tree.push_back(leaf("Intro", 2));
        const std::vector<NamedDestination> names = bookmarksToNamedDestinations(tree);
        QCOMPARE(names.size(), std::size_t{3});
        QVERIFY(names[0].name != names[1].name);
        QVERIFY(names[1].name != names[2].name);
    }

    void namesToBookmarks() {
        std::vector<NamedDestination> names;
        names.push_back(NamedDestination{"later", Destination::fitPage(5)});
        names.push_back(NamedDestination{"earlier", Destination::fitPage(1)});
        const BookmarkTree tree = bookmarksFromNamedDestinations(names);
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("earlier"));
        QCOMPARE(tree[0].target.kind, TargetKind::Named);

        QVERIFY(bookmarksFromNamedDestinations({}).empty());
    }

    void nameSanitizationDropsUnsafeBytes() {
        QCOMPARE(QString::fromStdString(sanitizeDestinationName("Chapter 1: (start)")),
                 QStringLiteral("Chapter_1_start"));
        QCOMPARE(QString::fromStdString(sanitizeDestinationName("   ")), QStringLiteral("dest"));
    }

    void unresolvedNamesAreLeftAlone() {
        BookmarkTree tree;
        tree.push_back(leaf("x", 0));
        tree[0].target = BookmarkTarget::named("missing");
        QCOMPARE(resolveNamedTargets(tree, {}), std::size_t{0});
        QCOMPARE(tree[0].target.kind, TargetKind::Named);
    }

    // -----------------------------------------------------------------------
    // PRD-BM-005 目錄頁版面、PRD-BM-008 由書籤建連結
    // -----------------------------------------------------------------------

    void tocLayoutPlacesLinesInReadingOrder() {
        const BookmarkTree tree = sampleTree();
        const TocLayout layout = layoutTableOfContents(tree);
        QCOMPARE(layout.pages.size(), std::size_t{1});
        QCOMPARE(layout.lineCount(), std::size_t{7});  // 標題列 + 6 個書籤

        const TocPage& page = layout.pages[0];
        QVERIFY(page.lines[0].isHeading);
        QCOMPARE(QString::fromStdString(page.lines[1].text), QStringLiteral("Chapter One"));
        QCOMPARE(page.lines[1].targetPageIndex, 0);
        QCOMPARE(QString::fromStdString(page.lines[1].pageLabel), QStringLiteral("1"));

        // 縮排要隨層級加深，基線要逐行往下。
        QVERIFY(page.lines[2].xPt > page.lines[1].xPt);
        QVERIFY(page.lines[2].baselineYPt < page.lines[1].baselineYPt);
        // 引導點的起點在標題右側、終點在頁碼左側。
        QVERIFY(page.lines[1].leaderStartXPt > page.lines[1].xPt);
        QVERIFY(page.lines[1].leaderEndXPt <= page.lines[1].pageLabelXPt);
    }

    void tocLayoutSpillsToSecondPage() {
        BookmarkTree tree;
        for (int i = 0; i < 200; ++i) tree.push_back(leaf("Entry " + std::to_string(i), i));
        const TocLayout layout = layoutTableOfContents(tree);
        QVERIFY2(layout.pages.size() >= 2, "200 個書籤應該排不進一頁");
        QCOMPARE(layout.lineCount(), std::size_t{201});
        // 每一行都必須在頁面內，否則使用者看到的是「後面的書籤不見了」。
        for (const TocPage& page : layout.pages) {
            for (const TocLine& line : page.lines) {
                QVERIFY(line.baselineYPt > 0.0);
                QVERIFY(line.baselineYPt < layout.pageHeightPt);
            }
        }
    }

    void tocLayoutEdgeCases() {
        const TocLayout empty = layoutTableOfContents(BookmarkTree{});
        QCOMPARE(empty.lineCount(), std::size_t{1});  // 只剩標題列

        TocLayoutOptions noHeading;
        noHeading.heading.clear();
        QCOMPARE(layoutTableOfContents(BookmarkTree{}, noHeading).lineCount(), std::size_t{0});

        // 邊界大於頁面時要回傳空版面，而不是排出畫在頁外的行。
        TocLayoutOptions absurd;
        absurd.marginPt = 400.0;
        QVERIFY(layoutTableOfContents(sampleTree(), absurd).pages.empty());

        // 深巢狀超過 maxDepth 的節點要被略過。
        TocLayoutOptions shallow;
        shallow.maxDepth = 2;
        shallow.heading.clear();
        QCOMPARE(layoutTableOfContents(sampleTree(), shallow).lineCount(), std::size_t{5});
    }

    void linksFollowTocLines() {
        const TocLayout layout = layoutTableOfContents(sampleTree());
        const std::vector<BookmarkLink> links = linksFromTocLayout(layout, 7);
        QCOMPARE(links.size(), std::size_t{6});  // 標題列沒有目標
        QCOMPARE(links[0].pageIndex, 7);
        QCOMPARE(links[0].destination.pageIndex, 0);
        QVERIFY(!links[0].rect.isEmpty());

        QVERIFY(linksFromTocLayout(TocLayout{}, 0).empty());
    }

    // -----------------------------------------------------------------------
    // PRD-BM-009 / 010 匯出
    // -----------------------------------------------------------------------

    void exportText() {
        const std::string text = exportAsText(sampleTree());
        const QStringList lines = QString::fromStdString(text).split(QLatin1Char('\n'));
        QCOMPARE(lines[0], QStringLiteral("Chapter One\t1"));
        QCOMPARE(lines[1], QStringLiteral("    Section 1.1\t2"));
        QCOMPARE(lines[2], QStringLiteral("        Sub 1.1.1\t3"));

        QVERIFY(exportAsText(BookmarkTree{}).empty());
    }

    void exportTextRoundTripsThroughParser() {
        // 匯出與匯入互為反向，是「匯出改一改再匯入」這個流程的前提。
        const BookmarkTree original = sampleTree();
        const BookmarkTree parsed = bookmarksFromText(exportAsText(original));
        QCOMPARE(titlesInOrder(parsed), titlesInOrder(original));
        QCOMPARE(treeDepth(parsed), treeDepth(original));
        QCOMPARE(parsed[0].children[0].children[0].target.destination.pageIndex, 2);
    }

    void exportHtmlIsWellNested() {
        const std::string html = exportAsHtml(sampleTree());
        const QString text = QString::fromStdString(html);
        QCOMPARE(text.count(QStringLiteral("<ul>")), text.count(QStringLiteral("</ul>")));
        QCOMPARE(static_cast<int>(text.count(QStringLiteral("<li>"))), 6);
        QVERIFY(text.contains(QStringLiteral("<li>Chapter One")));

        BookmarkTree escaping;
        escaping.push_back(leaf("A & B <tag> \"q\"", 0));
        const QString escaped = QString::fromStdString(exportAsHtml(escaping));
        QVERIFY(escaped.contains(QStringLiteral("A &amp; B &lt;tag&gt; &quot;q&quot;")));
        QVERIFY(!escaped.contains(QStringLiteral("<tag>")));

        const QString emptyHtml = QString::fromStdString(exportAsHtml(BookmarkTree{}));
        QVERIFY(emptyHtml.contains(QStringLiteral("</html>")));
        QCOMPARE(static_cast<int>(emptyHtml.count(QStringLiteral("<li>"))), 0);
    }

    void exportHtmlHandlesDeepChain() {
        const QString html = QString::fromStdString(exportAsHtml(deepChain(10)));
        QCOMPARE(static_cast<int>(html.count(QStringLiteral("<ul>"))), 10);
        QCOMPARE(static_cast<int>(html.count(QStringLiteral("</ul>"))), 10);
    }

    // -----------------------------------------------------------------------
    // PRD-BM-012 / 013 由目錄頁與文字檔產生書籤
    // -----------------------------------------------------------------------

    void parseTocText() {
        const std::string page =
            "Table of Contents\n"
            "Chapter One ......... 1\n"
            "    Section 1.1 ..... 2\n"
            "    Section 1.2 ..... 5\n"
            "Chapter Two ......... 9\n"
            "12\n";  // 頁尾頁碼，必須被濾掉

        const BookmarkTree tree = bookmarksFromTocText(page);
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Chapter One"));
        QCOMPARE(tree[0].children.size(), std::size_t{2});
        QCOMPARE(tree[0].children[1].target.destination.pageIndex, 4);
        // "Table of Contents" 沒有頁碼，requirePageNumber 會擋掉它。
        QCOMPARE(nodeCount(tree), std::size_t{4});
    }

    void parseTocTextHonoursPageOffset() {
        TextOutlineParseOptions options;
        options.pageIndexOffset = 2;  // 正文第 1 頁其實是檔案第 3 頁
        const BookmarkTree tree = bookmarksFromTocText("Intro .... 1\n", options);
        QCOMPARE(tree[0].target.destination.pageIndex, 2);
    }

    void parseTextFileWithTabs() {
        const std::string text =
            "Chapter One\t1\n"
            "\tSection 1.1\t2\n"
            "\t\tSub 1.1.1\t3\n"
            "Chapter Two\t4\n";
        const BookmarkTree tree = bookmarksFromText(text);
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(treeDepth(tree), 3);
        QCOMPARE(tree[0].children[0].children[0].target.destination.pageIndex, 2);
    }

    void parseTextEdgeCases() {
        QVERIFY(bookmarksFromText("").empty());
        QVERIFY(bookmarksFromText("\n\n   \n").empty());

        // 沒有頁碼的行在一般模式下仍是合法書籤（只是沒有目標）。
        const BookmarkTree noPages = bookmarksFromText("Alpha\n  Beta\n");
        QCOMPARE(noPages.size(), std::size_t{1});
        QCOMPARE(noPages[0].target.kind, TargetKind::None);

        // 深層縮排不得超過 maxDepth 而讓節點消失。
        std::string deep;
        for (int i = 0; i < 12; ++i) deep += std::string(static_cast<std::size_t>(i) * 2, ' ') +
                                             "L" + std::to_string(i) + "\t" +
                                             std::to_string(i + 1) + "\n";
        TextOutlineParseOptions options;
        options.maxDepth = 8;
        const BookmarkTree tree = bookmarksFromText(deep, options);
        QCOMPARE(nodeCount(tree), std::size_t{12});
        QCOMPARE(treeDepth(tree), 8);
    }

    void parseHandlesCrLf() {
        const BookmarkTree tree = bookmarksFromText("A\t1\r\n  B\t2\r\n");
        QCOMPARE(tree.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(tree[0].children[0].title), QStringLiteral("B"));
    }

    // -----------------------------------------------------------------------
    // PRD-BM-014 由高亮產生書籤
    // -----------------------------------------------------------------------

    void bookmarksFromHighlightsSortByReadingOrder() {
        std::vector<HighlightSource> highlights;
        highlights.push_back(HighlightSource{1, "second page", 700.0, 0.0});
        highlights.push_back(HighlightSource{0, "lower on page one", 200.0, 0.0});
        highlights.push_back(HighlightSource{0, "upper on page one", 700.0, 0.0});

        const BookmarkTree tree = bookmarksFromHighlights(highlights);
        QCOMPARE(tree.size(), std::size_t{3});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("upper on page one"));
        QCOMPARE(QString::fromStdString(tree[1].title), QStringLiteral("lower on page one"));
        QCOMPARE(tree[0].target.destination.zoom, ZoomType::XYZ);
        QCOMPARE(*tree[0].target.destination.top, 700.0);
    }

    void bookmarksFromHighlightsGrouped() {
        std::vector<HighlightSource> highlights;
        highlights.push_back(HighlightSource{0, "a", 700.0, 0.0});
        highlights.push_back(HighlightSource{0, "b", 600.0, 0.0});
        highlights.push_back(HighlightSource{2, "c", 500.0, 0.0});
        FromHighlightsOptions options;
        options.groupByPage = true;
        const BookmarkTree tree = bookmarksFromHighlights(highlights, options);
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Page 1"));
        QCOMPARE(tree[0].children.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[1].title), QStringLiteral("Page 3"));
    }

    void highlightTitlesAreTruncatedOnCharacterBoundaries() {
        std::vector<HighlightSource> highlights;
        // 五個中文字，截到三個字時不得切在 UTF-8 續接位元組上。
        highlights.push_back(HighlightSource{
            0, "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94", 100.0, 0.0});
        FromHighlightsOptions options;
        options.maxTitleChars = 3;
        const BookmarkTree tree = bookmarksFromHighlights(highlights, options);
        const QString title = QString::fromStdString(tree[0].title);
        QCOMPARE(title, QString::fromStdString("\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89..."));
        QVERIFY(!title.contains(QChar(QChar::ReplacementCharacter)));

        QVERIFY(bookmarksFromHighlights({}).empty());
    }

    // -----------------------------------------------------------------------
    // PRD-BM-015 由頁面文字產生書籤
    // -----------------------------------------------------------------------

    void headingsAreDetectedByFontSize() {
        std::vector<TextLine> lines;
        const auto body = [&lines](std::int32_t page, const char* text, double top) {
            lines.push_back(TextLine{page, text, 10.0, top, false});
        };
        lines.push_back(TextLine{0, "Chapter One", 20.0, 780.0, true});
        body(0, "body text that goes on and on for a while", 760.0);
        lines.push_back(TextLine{0, "Section 1.1", 14.0, 700.0, true});
        body(0, "more body text here to weigh down the histogram", 680.0);
        body(0, "even more body text so ten point wins the vote", 660.0);
        lines.push_back(TextLine{1, "Chapter Two", 20.0, 780.0, true});
        body(1, "closing body text with plenty of characters", 760.0);

        const BookmarkTree tree = bookmarksFromPageText(lines);
        QCOMPARE(tree.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(tree[0].title), QStringLiteral("Chapter One"));
        QCOMPARE(tree[0].children.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(tree[0].children[0].title), QStringLiteral("Section 1.1"));
        QCOMPARE(tree[1].target.destination.pageIndex, 1);
        QCOMPARE(*tree[0].target.destination.top, 780.0);
    }

    void headingDetectionEdgeCases() {
        QVERIFY(bookmarksFromPageText({}).empty());

        // 全部同一個字級：沒有任何行比內文大，結果應該是空樹而不是把整頁變書籤。
        std::vector<TextLine> uniform;
        for (int i = 0; i < 5; ++i) {
            uniform.push_back(TextLine{0, "line " + std::to_string(i), 11.0, 700.0 - i * 12.0});
        }
        QVERIFY(bookmarksFromPageText(uniform).empty());

        // 過長的行是段落不是標題，即使字級夠大也不收。
        std::vector<TextLine> longLine;
        longLine.push_back(TextLine{0, "a b c d e f g h i j k l m n o p q r s t u v", 24.0, 700.0});
        for (int i = 0; i < 4; ++i) {
            longLine.push_back(TextLine{0, "ordinary body text line", 10.0, 600.0 - i * 12.0});
        }
        QVERIFY(bookmarksFromPageText(longLine).empty());
    }

    // -----------------------------------------------------------------------
    // PRD-BM-018 依書籤排序頁面
    // -----------------------------------------------------------------------

    void pageOrderFollowsBookmarks() {
        BookmarkTree tree;
        tree.push_back(leaf("third", 2));
        tree.push_back(leaf("first", 0));
        const PageOrderResult result = pageOrderFromBookmarks(tree, 4);
        const std::vector<std::int32_t> expected{2, 0, 1, 3};
        QCOMPARE(result.order, expected);
        const std::vector<std::int32_t> unreferenced{1, 3};
        QCOMPARE(result.unreferenced, unreferenced);
        QVERIFY(!result.identity);
    }

    void pageOrderEdgeCases() {
        QVERIFY(pageOrderFromBookmarks(sampleTree(), 0).order.empty());

        // 書籤已是自然順序時要回報 identity，讓呼叫端省下整次寫入。
        BookmarkTree ordered;
        for (int i = 0; i < 3; ++i) ordered.push_back(leaf("p", i));
        QVERIFY(pageOrderFromBookmarks(ordered, 3).identity);

        // 越界與重複的目標不得產生重複或缺漏的頁序。
        BookmarkTree odd;
        odd.push_back(leaf("dup", 1));
        odd.push_back(leaf("dup", 1));
        odd.push_back(leaf("out", 99));
        const PageOrderResult result = pageOrderFromBookmarks(odd, 3);
        const std::vector<std::int32_t> expected{1, 0, 2};
        QCOMPARE(result.order, expected);
    }

    // -----------------------------------------------------------------------
    // PRD-BM-019 驗證書籤
    // -----------------------------------------------------------------------

    void validationFindsBrokenTargets() {
        BookmarkTree tree;
        tree.push_back(leaf("ok", 0));
        tree.push_back(leaf("too far", 42));
        tree.push_back(Bookmark{});  // 空標題 + 沒有目標
        tree.push_back(leaf("named", 0));
        tree.back().target = BookmarkTarget::named("nowhere");

        ValidationOptions options;
        options.pageCount = 3;
        const std::vector<ValidationIssue> issues = validate(tree, options);

        const auto count = [&issues](IssueKind kind) {
            return static_cast<int>(std::count_if(
                issues.begin(), issues.end(),
                [kind](const ValidationIssue& i) { return i.kind == kind; }));
        };
        QCOMPARE(count(IssueKind::PageOutOfRange), 1);
        QCOMPARE(count(IssueKind::EmptyTitle), 1);
        QCOMPARE(count(IssueKind::MissingTarget), 1);
        QCOMPARE(count(IssueKind::UnresolvedName), 1);

        // 指向不存在頁面的那一項要能被定位回去。
        for (const ValidationIssue& issue : issues) {
            if (issue.kind != IssueKind::PageOutOfRange) continue;
            const BookmarkPath expected{1};
            QCOMPARE(issue.path, expected);
            QCOMPARE(QString::fromStdString(issue.title), QStringLiteral("too far"));
            QVERIFY(!issue.detail.empty());
        }
    }

    void validationCleanTreeHasNoIssues() {
        ValidationOptions options;
        options.pageCount = 6;
        QVERIFY(validate(sampleTree(), options).empty());
        QVERIFY(validate(BookmarkTree{}, options).empty());
    }

    void validationFlagsDepthAndDuplicates() {
        ValidationOptions options;
        options.pageCount = 20;
        options.maxDepth = 8;
        const std::vector<ValidationIssue> deep = validate(deepChain(10), options);
        QCOMPARE(static_cast<int>(std::count_if(deep.begin(), deep.end(),
                                                [](const ValidationIssue& i) {
                                                    return i.kind == IssueKind::ExcessiveDepth;
                                                })),
                 2);  // 第 9、10 層

        BookmarkTree duplicates;
        duplicates.push_back(leaf("same", 1));
        duplicates.push_back(leaf("same", 1));
        const std::vector<ValidationIssue> issues = validate(duplicates, options);
        QCOMPARE(static_cast<int>(std::count_if(issues.begin(), issues.end(),
                                                [](const ValidationIssue& i) {
                                                    return i.kind == IssueKind::DuplicateSibling;
                                                })),
                 1);
    }

    void outlineNodeConversionKeepsHierarchy() {
        std::vector<OutlineNode> outline;
        OutlineNode root;
        root.title = "Root";
        root.pageIndex = 1;
        OutlineNode child;
        child.title = "Child";
        child.pageIndex = 2;
        child.destination = PointF{10.0, 500.0};
        child.zoom = 1.5;
        root.children.push_back(child);
        outline.push_back(root);

        const BookmarkTree tree = fromOutline(outline);
        QCOMPARE(tree.size(), std::size_t{1});
        QCOMPARE(tree[0].target.destination.zoom, ZoomType::Fit);
        QCOMPARE(tree[0].children[0].target.destination.zoom, ZoomType::XYZ);
        QCOMPARE(*tree[0].children[0].target.destination.top, 500.0);
        QCOMPARE(*tree[0].children[0].target.destination.zoomFactor, 1.5);
    }

    void zoomTypeNamesRoundTrip() {
        const ZoomType all[] = {ZoomType::XYZ,  ZoomType::Fit,  ZoomType::FitH, ZoomType::FitV,
                                ZoomType::FitR, ZoomType::FitB, ZoomType::FitBH, ZoomType::FitBV};
        for (const ZoomType type : all) {
            ZoomType parsed{};
            QVERIFY(zoomTypeFromName(zoomTypeName(type), parsed));
            QCOMPARE(parsed, type);
        }
        ZoomType unused{};
        QVERIFY(!zoomTypeFromName("Nonsense", unused));
    }
};

QTEST_APPLESS_MAIN(TestBookmarkOps)
#include "test_bookmark_ops.moc"
