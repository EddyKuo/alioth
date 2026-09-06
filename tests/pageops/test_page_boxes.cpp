// 設定文件邊界（PRD-PAGE-010）與正規化頁面 / MediaBox 偏移（PRD-PAGE-011），WBS 12。
//
// 正規化是本工作包最容易寫錯的一項，因此這裡的斷言刻意分成兩個互不重疊的角度：
//
//   內容：以 PDFium 的文字層驗文字的絕對座標確實平移了。PDFium 報的是頁面空間的
//         絕對座標（實測，不是假設），因此正規化做對時文字會從 (50,100) 移到
//         (30,130)——相對於頁面框的位置不變。漏掉內容平移的實作會讓它留在原地。
//   註解：以物件層讀 /Rect 的絕對座標。文字層看不到註解，而漏搬註解的症狀
//         正是「頁面看起來完全正確、標記卻跑掉」——沒有這一條就驗不到。

#include <QtTest>

#include <QTemporaryDir>

#include <memory>

#include "engine/pageops/page_boxes.h"
#include "pageops_readback.h"
#include "qa/qpdf_check.h"

using namespace alioth;
using namespace alioth::engine::pageops;
using alioth::test::pageops::FixtureAnnotation;
using alioth::test::pageops::FixturePage;

namespace {

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// MediaBox 原點不在 (0,0) 的頁面。真實檔案裡這種頁面遠比想像中常見：
// 掃描器、排版軟體、裁切過的工程圖都會產生。
FixturePage offsetPage() {
    FixturePage page;
    page.media = domain::RectF{20.0, -30.0, 620.0, 812.0};  // 600×842
    page.text = "OFFSET";
    page.textAt = domain::PointF{50.0, 100.0};  // 相對於框原點是 (30,130)
    page.fontSize = 14.0;

    FixtureAnnotation annotation;
    annotation.rect = domain::RectF{40.0, 90.0, 140.0, 110.0};
    page.annotations.push_back(annotation);
    return page;
}

}  // namespace

class TestPageBoxes : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void allFiveBoxesAreWrittenAndReadBack() {
        FixturePage page;
        page.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
        page.text = "BOXES";
        const std::string source = test::pageops::makeFixturePdf({page});

        PageBoxRequest request;
        request.settings.media = domain::RectF{0.0, 0.0, 300.0, 500.0};
        request.settings.crop = domain::RectF{10.0, 10.0, 290.0, 490.0};
        request.settings.bleed = domain::RectF{5.0, 5.0, 295.0, 495.0};
        request.settings.trim = domain::RectF{20.0, 20.0, 280.0, 480.0};
        request.settings.art = domain::RectF{30.0, 30.0, 270.0, 470.0};

        const PageBoxResult result = setPageBoxes(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.changedPages, 1);
        QVERIFY(!result.clamped);

        const struct {
            const char* key;
            domain::RectF expected;
        } expected[] = {
            {"MediaBox", domain::RectF{0, 0, 300, 500}},
            {"CropBox", domain::RectF{10, 10, 290, 490}},
            {"BleedBox", domain::RectF{5, 5, 295, 495}},
            {"TrimBox", domain::RectF{20, 20, 280, 480}},
            {"ArtBox", domain::RectF{30, 30, 270, 470}},
        };
        for (const auto& item : expected) {
            domain::RectF box{};
            QVERIFY2(test::pageops::readPageBox(result.bytes, 0, item.key, box), item.key);
            QCOMPARE(box, item.expected);
        }

        assertQpdfClean(result.bytes, QStringLiteral("boxes-all"));
    }

    void childBoxesAreClampedIntoMedia() {
        FixturePage page;
        page.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
        const std::string source = test::pageops::makeFixturePdf({page});

        PageBoxRequest request;
        request.settings.crop = domain::RectF{-50.0, -50.0, 250.0, 450.0};

        const PageBoxResult result = setPageBoxes(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QVERIFY2(result.clamped, "子框超出 MediaBox 卻沒有被夾住");

        domain::RectF crop{};
        QVERIFY(test::pageops::readPageBox(result.bytes, 0, "CropBox", crop));
        QCOMPARE(crop, (domain::RectF{0, 0, 200, 400}));

        // 要求嚴格把關時必須明確失敗，而不是悄悄夾好。
        PageBoxRequest strict = request;
        strict.settings.clampToMedia = false;
        const PageBoxResult rejected = setPageBoxes(source, strict);
        QCOMPARE(rejected.status, PageOpsStatus::LayoutRejected);
        QCOMPARE(rejected.compose, domain::compose::ComposeStatus::BoxOutsideMedia);
    }

    void unsetBoxesAndUnselectedPagesAreLeftAlone() {
        FixturePage first;
        first.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
        first.cropBox = domain::RectF{5.0, 5.0, 195.0, 395.0};
        FixturePage second = first;
        const std::string source = test::pageops::makeFixturePdf({first, second});

        PageBoxRequest request;
        request.pages = {1};
        request.settings.trim = domain::RectF{10.0, 10.0, 190.0, 390.0};

        const PageBoxResult result = setPageBoxes(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.changedPages, 1);

        // 沒被指定的框保持原樣：「只想改 TrimBox」時順手清掉 CropBox
        // 會毀掉印刷用的檔案。
        domain::RectF crop{};
        QVERIFY(test::pageops::readPageBox(result.bytes, 1, "CropBox", crop));
        QCOMPARE(crop, (domain::RectF{5, 5, 195, 395}));

        // 沒被指定的頁面完全不動。
        domain::RectF trim{};
        QVERIFY(!test::pageops::readPageBox(result.bytes, 0, "TrimBox", trim));
        QVERIFY(test::pageops::readPageBox(result.bytes, 1, "TrimBox", trim));

        // 什麼都不指定時明確拒絕，而不是輸出一份沒有任何差別的新檔。
        QCOMPARE(setPageBoxes(source, PageBoxRequest{}).status, PageOpsStatus::InvalidRequest);
    }

    void normalizationMovesContentAndAnnotationsTogether() {
        const std::string source = test::pageops::makeFixturePdf({offsetPage()});

        // 前提檢查：PDFium 的文字座標是頁面空間的**絕對**座標，不隨 MediaBox
        // 原點調整。因此正規化之前文字在 (50,100)，之後應該在 (30,130)——
        // 也就是相對於頁面框的位置沒有變。少了這一步，下面兩條斷言有可能
        // 只是因為文字層本來就抽不出東西而通過。
        const std::string before = test::pageops::textInArea(source, dir_->path(), 0,
                                                             domain::RectF{40, 90, 220, 120});
        QVERIFY2(contains(before, "OFFSET"), before.c_str());

        NormalizeResult result = normalizePages(source, NormalizeRequest{});
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.normalizedPages, 1);
        QCOMPARE(result.movedAnnotations, 1);

        // 一、MediaBox 的原點移到 (0,0)，尺寸不變。
        domain::RectF media{};
        QVERIFY(test::pageops::readPageBox(result.bytes, 0, "MediaBox", media));
        QCOMPARE(media, (domain::RectF{0, 0, 600, 842}));

        // 二、內容跟著平移 (-20,+30)：相對於頁面框的位置不變，絕對座標變了。
        // 漏掉內容平移的實作會讓文字留在 (50,100)，而頁面框已經移走。
        const std::string after = test::pageops::textInArea(result.bytes, dir_->path(), 0,
                                                            domain::RectF{20, 120, 200, 150});
        QVERIFY2(contains(after, "OFFSET"),
                 ("正規化之後內容沒有跟著平移，讀到：" + after).c_str());
        const std::string oldPlace = test::pageops::textInArea(result.bytes, dir_->path(), 0,
                                                               domain::RectF{40, 90, 220, 115});
        QVERIFY2(!contains(oldPlace, "OFFSET"), oldPlace.c_str());

        // 三、註解跟著平移。這一條是整個 PRD-PAGE-011 最容易漏的地方，
        // 漏了的話頁面看起來完全正確，只有把註解點開才會發現它跑掉了。
        const std::vector<test::pageops::AnnotationReadback> annotations =
            test::pageops::readAnnotations(result.bytes, 0);
        QCOMPARE(annotations.size(), std::size_t{1});
        QCOMPARE(annotations[0].rect, (domain::RectF{20, 120, 120, 140}));
        QCOMPARE(annotations[0].quadPoints.size(), std::size_t{8});
        QCOMPARE(annotations[0].quadPoints[0], 20.0);   // 左上 x
        QCOMPARE(annotations[0].quadPoints[1], 140.0);  // 左上 y

        assertQpdfClean(result.bytes, QStringLiteral("normalize"));
    }

    void normalizationShiftsEveryChildBox() {
        FixturePage page = offsetPage();
        page.cropBox = domain::RectF{30.0, -20.0, 610.0, 800.0};
        const std::string source = test::pageops::makeFixturePdf({page});

        const NormalizeResult result = normalizePages(source, NormalizeRequest{});
        QVERIFY2(result.ok(), result.diagnostic.c_str());

        // CropBox 沒有跟著平移的話，頁面會被裁在錯誤的位置，
        // 而那看起來像是「內容被平移錯了」，會把人引到完全錯誤的方向。
        domain::RectF crop{};
        QVERIFY(test::pageops::readPageBox(result.bytes, 0, "CropBox", crop));
        QCOMPARE(crop, (domain::RectF{10, 10, 590, 830}));
    }

    void alreadyNormalPagesAreNotRewritten() {
        FixturePage page;
        page.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
        page.text = "PLAIN";
        const std::string source = test::pageops::makeFixturePdf({page});

        const NormalizeResult result = normalizePages(source, NormalizeRequest{});
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        // 已經在原點的頁面不重寫：每次開檔存檔都讓檔案長大一段，
        // 使用者會（正確地）認為我們在偷改他的檔案。
        QCOMPARE(result.normalizedPages, 0);
        QCOMPARE(result.movedAnnotations, 0);
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 0), "PLAIN"));
    }

    void normalizationLeavesOtherPagesAlone() {
        FixturePage plain;
        plain.media = domain::RectF{0.0, 0.0, 200.0, 400.0};
        plain.text = "PLAIN";
        const std::string source = test::pageops::makeFixturePdf({offsetPage(), plain});

        NormalizeRequest request;
        request.pages = {0};

        const NormalizeResult result = normalizePages(source, request);
        QVERIFY2(result.ok(), result.diagnostic.c_str());
        QCOMPARE(result.pageCount, 2);

        domain::RectF media{};
        QVERIFY(test::pageops::readPageBox(result.bytes, 1, "MediaBox", media));
        QCOMPARE(media, (domain::RectF{0, 0, 200, 400}));
        QVERIFY(contains(test::pageops::pageText(result.bytes, dir_->path(), 1), "PLAIN"));

        QCOMPARE(normalizePages(source, NormalizeRequest{{5}, true}).status,
                 PageOpsStatus::PageOutOfRange);
    }

private:
    void assertQpdfClean(const std::string& bytes, const QString& name) {
        const QString path = dir_->path() + QStringLiteral("/%1.pdf").arg(name);
        QVERIFY(test::pageops::writeBytes(path, bytes));
        const test::QpdfCheckResult check = test::runQpdfCheck(path);
        if (check.status == test::QpdfStatus::NotAvailable) QSKIP("找不到 qpdf，結構檢查略過");
        QVERIFY2(check.clean(), check.output.toUtf8().constData());
    }

    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestPageBoxes)
#include "test_page_boxes.moc"
