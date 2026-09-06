// qpdf 結構檢查（PRD-IO-001 的明列驗收條件、PRD §9）。
//
// 本檔刻意不修改既有的 tests/save 與 tests/annotations，而是自己把同樣的產出路徑
// 再走一次：那些測試驗的是「PDFium 讀得回來」，這裡驗的是「一個獨立於 PDFium 的
// 解析器認為檔案結構是對的」。兩者是不同的問題，混在同一支測試裡會讓失敗訊息
// 難以歸因——PDFium 讀得回來但 qpdf 抱怨，代表我們寫壞了結構卻被 PDFium 的
// 容忍度掩蓋；那正是最值得被獨立標示出來的一類缺陷。
//
// 涵蓋的產出路徑：
//   1. 測試語料產生器本身（tools/corpus）——它產出的 xref 若是錯的，
//      後面所有以它為基礎的結論都不成立
//   2. AnnotationDocument::saveIncremental（記憶體位元組，九種註解型別）
//   3. IncrementalSaver::saveIncremental（就地存檔，走暫存檔與原子更名）
//   4. IncrementalSaver::saveAsCopy（整份重寫的對照組）
//   5. AnnotationService::addHighlight（應用層的核心迴圈端到端）
//
// qpdf 不存在時每一項都 skip 並說明原因，不會靜默通過。

#include <QtTest>

#include <QByteArray>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <string>
#include <vector>

#include "app/annotation_service.h"
#include "app/selection_controller.h"
#include "domain/annotation.h"
#include "engine/annotations/annotation_document.h"
#include "engine/save/incremental_saver.h"
#include "engineering_corpus.h"
#include "qpdf_check.h"
#include "save_fixture.h"
#include "text_pdf_fixture.h"

using namespace alioth;
using alioth::test::describeQpdfFailure;
using alioth::test::findQpdf;
using alioth::test::qpdfSkipReason;
using alioth::test::QpdfCheckResult;
using alioth::test::runQpdfCheck;
using alioth::test::runQpdfCheckOnBytes;

namespace {

// 九種註解型別各一則。型別由幾何決定（domain::AnnotationGeometry 是 variant），
// 所以「所有型別都寫得出結構正確的 PDF」這件事只能靠列舉全部幾何來驗。
std::vector<domain::Annotation> makeAllAnnotationKinds() {
    using namespace alioth::domain;
    std::vector<Annotation> list;

    const auto base = [](const char* id) {
        Annotation a{};
        a.id = id;
        a.color = ColorRgb{0.9, 0.2, 0.2};
        a.opacity = 0.6;
        a.author = "QA";
        a.contents = "結構檢查用";
        a.creationDate = PdfDate{2026, 9, 5, 10, 0, 0, 8, 0};
        a.modifiedDate = PdfDate{2026, 9, 5, 10, 0, 0, 8, 0};
        a.flags = AnnotationFlag::Print;
        return a;
    };

    for (const auto kind : {TextMarkupKind::Highlight, TextMarkupKind::Underline,
                            TextMarkupKind::StrikeOut, TextMarkupKind::Squiggly}) {
        Annotation a = base("qa-markup");
        a.geometry = TextMarkupGeometry{kind, {quadFromPageRect(RectF{20.0, 300.0, 180.0, 320.0})}};
        list.push_back(a);
    }

    for (const auto kind : {ShapeKind::Square, ShapeKind::Circle}) {
        Annotation a = base("qa-shape");
        a.rect = RectF{30.0, 100.0, 150.0, 200.0};
        a.interiorColor = ColorRgb{0.2, 0.4, 0.9};
        a.geometry = ShapeGeometry{kind};
        list.push_back(a);
    }

    {
        Annotation a = base("qa-line");
        a.rect = RectF{20.0, 40.0, 180.0, 90.0};
        a.geometry = LineGeometry{PointF{25.0, 45.0}, PointF{175.0, 85.0}, LineEnding::OpenArrow,
                                  LineEnding::ClosedArrow};
        list.push_back(a);
    }
    {
        Annotation a = base("qa-ink");
        a.rect = RectF{20.0, 220.0, 180.0, 280.0};
        a.geometry = InkGeometry{{{PointF{25.0, 225.0}, PointF{60.0, 270.0}, PointF{120.0, 230.0},
                                   PointF{175.0, 275.0}}}};
        list.push_back(a);
    }
    {
        Annotation a = base("qa-note");
        a.rect = RectF{150.0, 350.0, 170.0, 370.0};
        a.geometry = TextNoteGeometry{TextNoteIcon::Comment, false};
        list.push_back(a);
    }

    return list;
}

}  // namespace

class TestQpdfStructure : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());

        const QString qpdf = findQpdf();
        if (qpdf.isEmpty()) {
            // 這行是刻意的：即使全部 skip，日誌裡也必須留下「這一輪沒有驗結構」的痕跡。
            qWarning("%s", qpdfSkipReason().constData());
        } else {
            qInfo("使用 qpdf: %s", qPrintable(qpdf));
        }
    }

    // 語料產生器自己先過關。它若寫出壞掉的 xref，後面每一項的失敗都會歸因錯誤。
    void syntheticCorpusIsStructurallyValid() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        corpus::CorpusOptions options = corpus::goldenCorpusOptions();
        options.pageCount = 3;
        const std::string bytes = corpus::generate(options);

        const QString path = dir_->filePath(QStringLiteral("corpus.pdf"));
        const QpdfCheckResult result = runQpdfCheckOnBytes(
            path, QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
        QVERIFY2(result.clean(), describeQpdfFailure(QStringLiteral("合成工程圖語料"), result));
    }

    // 含嵌入影像的語料另外驗一次：影像 XObject 的 /Length 算錯是最典型的
    // 「PDFium 照樣讀得出來、qpdf 會抱怨」的缺陷。
    void syntheticCorpusWithImageIsStructurallyValid() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        corpus::CorpusOptions options = corpus::goldenCorpusOptions();
        options.pageCount = 2;
        options.embedImage = true;
        options.imageEdgePixels = 64;
        const std::string bytes = corpus::generate(options);

        const QString path = dir_->filePath(QStringLiteral("corpus_image.pdf"));
        const QpdfCheckResult result = runQpdfCheckOnBytes(
            path, QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
        QVERIFY2(result.clean(), describeQpdfFailure(QStringLiteral("含影像語料"), result));
    }

    // 九種註解型別各寫一則後增量儲存。這是 WBS 4.1–4.3 的產出物，
    // 也是「產出的 PDF 要通過 qpdf 結構檢查」這條驗收條件最直接的對象。
    void incrementalSaveWithEveryAnnotationKindPassesCheck() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        const QByteArray source = test::makeTextPdf();
        engine::annotations::AnnotationDocument document;
        QVERIFY(document.openFromMemory(source.constData(),
                                        static_cast<std::size_t>(source.size())));

        // PDFium 的 FPDFAnnot_IsSupportedSubtype 並不接受全部九種型別
        // （實測 154.0.8035：/Line 被拒）。這是引擎的既有限制，不是本測試要修的事，
        // 但也不能靜默略過——被拒的型別列出來，讓「哪些型別根本寫不進檔案」
        // 這件事在每次執行的日誌裡都看得見。
        int accepted = 0;
        QStringList refused;
        for (const domain::Annotation& annotation : makeAllAnnotationKinds()) {
            const auto written = document.addAnnotation(0, annotation);
            if (written.ok) {
                ++accepted;
            } else {
                refused << QStringLiteral("%1（%2）")
                               .arg(static_cast<int>(annotation.type()))
                               .arg(QString::fromStdString(written.diagnostic));
            }
        }
        if (!refused.isEmpty()) {
            qWarning("PDFium 拒絕建立的註解型別（既有限制，見 status.md）：%s",
                     qPrintable(refused.join(QStringLiteral("; "))));
        }
        // 至少要有寫得進去的型別，否則這項測試等於什麼都沒驗。
        QVERIFY2(accepted >= 8,
                 qPrintable(QStringLiteral("只有 %1 種註解型別寫得進去，預期至少 8 種")
                                .arg(accepted)));

        const std::vector<unsigned char> saved = document.saveIncremental();
        QVERIFY(!saved.empty());

        const QByteArray bytes(reinterpret_cast<const char*>(saved.data()),
                               static_cast<qsizetype>(saved.size()));
        // 增量的前提：原檔位元組原封不動。這條在 tests/annotations 已驗過，
        // 這裡重驗一次是為了讓 qpdf 的失敗不會被誤讀成「增量壞了」。
        QCOMPARE(bytes.left(source.size()), source);

        const QString path = dir_->filePath(QStringLiteral("all_kinds.pdf"));
        const QpdfCheckResult result = runQpdfCheckOnBytes(path, bytes);
        QVERIFY2(result.clean(), describeQpdfFailure(QStringLiteral("九種註解增量儲存"), result));
    }

    // 就地存檔：走暫存檔 → 落盤同步 → 原子更名的完整路徑（tests/save 的產出物）。
    void inPlaceIncrementalSavePassesCheck() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        const QString path = dir_->filePath(QStringLiteral("inplace.pdf"));
        QVERIFY(test::writePdfTo(path, test::makeBulkyPdf(3, 40000)));

        engine::save::ScopedDocument document;
        QVERIFY(document.open(path.toStdString()));
        const auto result = engine::save::IncrementalSaver::saveIncremental(
            document.handle(), path.toStdString(), path.toStdString());
        QVERIFY2(result.ok(), engine::save::describe(result.status));
        document.close();

        const QpdfCheckResult check = runQpdfCheck(path);
        QVERIFY2(check.clean(), describeQpdfFailure(QStringLiteral("就地增量儲存"), check));
    }

    // 另存新檔（整份重寫）。它會重排物件編號，是結構最容易出問題的一條路徑。
    void saveAsCopyPassesCheck() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        const QString source = dir_->filePath(QStringLiteral("copy_source.pdf"));
        const QString target = dir_->filePath(QStringLiteral("copy_target.pdf"));
        QVERIFY(test::writePdfTo(source, test::makeBulkyPdf(3, 40000)));

        engine::save::ScopedDocument document;
        QVERIFY(document.open(source.toStdString()));
        const auto result =
            engine::save::IncrementalSaver::saveAsCopy(document.handle(), target.toStdString());
        QVERIFY2(result.ok(), engine::save::describe(result.status));
        document.close();

        const QpdfCheckResult check = runQpdfCheck(target);
        QVERIFY2(check.clean(), describeQpdfFailure(QStringLiteral("另存新檔"), check));
    }

    // 應用層的核心迴圈端到端：選字 → 螢光筆 → 增量儲存 → 檔案。
    // 前面幾項驗的是各子系統，這一項驗的是使用者實際會產生的那個檔案。
    void highlightFlowOutputPassesCheck() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        const QString path = dir_->filePath(QStringLiteral("highlight.pdf"));
        QVERIFY(test::writePdfTo(path, test::makeTextPdf()));

        app::SelectionController selection;
        QSignalSpy changed(&selection, &app::SelectionController::selectionChanged);
        selection.openDocument(path);
        selection.selectWordAt(0, domain::PointF{60.0, 404.0});
        QVERIFY2(changed.wait(10000), "選取沒有回報");
        QVERIFY(!selection.selection().isEmpty());

        app::HighlightRequest request;
        request.path = path;
        request.pageIndex = selection.selection().pageIndex;
        request.quads = selection.selection().quads;
        request.author = QStringLiteral("QA");
        request.contents = selection.selection().text;

        app::AnnotationService service;
        const app::HighlightResult written = service.addHighlight(request);
        QVERIFY2(written.ok, qPrintable(written.message));

        const QpdfCheckResult check = runQpdfCheck(path);
        QVERIFY2(check.clean(), describeQpdfFailure(QStringLiteral("核心迴圈產出"), check));
    }

    // 反向驗證：故意破壞一份檔案，qpdf 必須抓到。
    //
    // 沒有這一項，「全部通過」也可能只是因為我們把 qpdf 的參數傳錯、
    // 它其實什麼都沒檢查。檢查工具本身也需要被檢查。
    void deliberatelyCorruptedFileIsRejected() {
        if (findQpdf().isEmpty()) QSKIP(qpdfSkipReason().constData());

        QByteArray bytes = test::makeTextPdf();
        const int xrefPos = bytes.lastIndexOf("startxref");
        QVERIFY(xrefPos > 0);
        // 把 startxref 的偏移量改成明顯錯誤的值。真實世界的損壞 xref 就長這樣。
        const int numberStart = bytes.indexOf('\n', xrefPos) + 1;
        const int numberEnd = bytes.indexOf('\n', numberStart);
        QVERIFY(numberEnd > numberStart);
        bytes.replace(numberStart, numberEnd - numberStart, "999999");

        const QString path = dir_->filePath(QStringLiteral("corrupt.pdf"));
        const QpdfCheckResult result = runQpdfCheckOnBytes(path, bytes);
        QVERIFY2(!result.clean(),
                 "刻意損壞的檔案竟然通過 qpdf --check，代表檢查根本沒有生效");
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestQpdfStructure)
#include "test_qpdf_structure.moc"
