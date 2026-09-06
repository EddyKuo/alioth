// PDFium 多文件使用方式的架構約束（ADR-005）。
//
// 這支測試釘住的是隔離實驗留下的結論，不是產品功能。實驗把兩個先前混在一起的
// 變因拆開量測，結果是：
//
//   多份文件把手同時存在，但嚴格輪流動作  → 完全正常（本檔第二條）
//   多條執行緒同時動作                    → 非決定性地掉資料：
//                                           兩個工作者第一輪 60/60、第二輪 5/60，
//                                           之後整個行程的開檔開始失敗
//
// 因此 SDD §1.1「各自持有把手與專用執行緒，所以可以並行」是錯的：
// 可以並存，不可以並行。PDFium 的行程級狀態（字型與模組快取）非執行緒安全，
// 而它壞掉的方式是安靜地少給資料，不是崩潰。
//
// 本檔只保留**必須恆真**的兩條。刻意不保留「並行會失敗」的斷言：
// 那等於把缺陷寫成規格，而且跑完之後行程狀態已經被污染，
// 後面的測試會拿到看起來毫不相干的錯誤。並行的證據留在 ADR-005。

#include <QtTest>

#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "engine/pdfium_engine.h"
#include "engine/text/text_extractor.h"

using namespace alioth;
using namespace alioth::engine::text;

namespace {

constexpr int kPages = 60;

QByteArray makeSearchablePdf(int pageCount) {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        kids += QByteArray::number(4 + i * 2) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                      QByteArray::number(pageCount) + " >>");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    for (int i = 0; i < pageCount; ++i) {
        const QByteArray text = "Alioth page " + QByteArray::number(i);
        const QByteArray content = "BT /F1 14 Tf 50 700 Td (" + text + ") Tj ET\n";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 800] /Contents " +
                          QByteArray::number(5 + i * 2) +
                          " 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
        objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                          content + "endstream");
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

struct Outcome {
    int openFailures{0};
    int pagesWithText{0};
    int pagesMissing{0};  // 回呼拿到 nullptr，或字元數為 0
};

// workerCount 條執行緒各自開一份文件（paths 依序取用，長度不足時循環），
// 以交錯方式分掉 kPages 頁，每頁只問「這頁有沒有文字」。
Outcome scanWith(const std::vector<std::string>& paths, int workerCount) {
    Outcome outcome;
    std::atomic<int> withText{0};
    std::atomic<int> missing{0};

    std::vector<std::unique_ptr<TextExtractor>> workers;
    std::vector<bool> usable;
    workers.reserve(static_cast<std::size_t>(workerCount));

    for (int i = 0; i < workerCount; ++i) {
        auto extractor = std::make_unique<TextExtractor>();
        // 開檔結果放在 shared_ptr 而不是堆疊上：等待逾時的話，回呼仍然可能在之後
        // 觸發，寫進已經離開作用域的堆疊就是寫進死掉的記憶體，而症狀會出現在
        // 後面某個不相干的地方。
        auto opened = std::make_shared<std::atomic<bool>>(false);
        const std::string& path = paths[static_cast<std::size_t>(i) % paths.size()];
        extractor->open(path, "", [opened](domain::DocumentError error) {
            opened->store(error == domain::DocumentError::None);
        });
        extractor->waitForIdle();
        usable.push_back(opened->load());
        if (!usable.back()) ++outcome.openFailures;
        workers.push_back(std::move(extractor));
    }

    for (int i = 0; i < workerCount; ++i) {
        if (!usable[static_cast<std::size_t>(i)]) continue;
        for (int page = i; page < kPages; page += workerCount) {
            workers[static_cast<std::size_t>(i)]->withTextPage(
                page, [&withText, &missing](const TextPage* textPage) {
                    if (textPage != nullptr && charCount(*textPage) > 0) {
                        withText.fetch_add(1);
                    } else {
                        missing.fetch_add(1);
                    }
                });
        }
    }

    for (auto& worker : workers) worker->waitForIdle();

    outcome.pagesWithText = withText.load();
    outcome.pagesMissing = missing.load();
    return outcome;
}

}  // namespace

class TestPdfiumConcurrency : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        const QByteArray bytes = makeSearchablePdf(kPages);
        // 四份內容完全相同、路徑不同的副本。內容相同才能把「檔案共用」
        // 從「語料差異」裡分離出來。
        for (int i = 0; i < 4; ++i) {
            const QString path = dir_->filePath(QStringLiteral("copy%1.pdf").arg(i));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
            file.close();
            copies_.push_back(path.toStdString());
        }
    }

    void singleWorkerScansEveryPage() {
        // 基準線。這一條要是不過，後面的比較全部沒有意義。
        for (int attempt = 0; attempt < 3; ++attempt) {
            const Outcome outcome = scanWith({copies_[0]}, 1);
            report("1 worker / same file", attempt, outcome);
            QCOMPARE(outcome.openFailures, 0);
            QCOMPARE(outcome.pagesWithText, kPages);
        }
    }

    // 四個擷取器同時「活著」，但嚴格輪流動作——任何時刻只有一條執行緒在碰 PDFium。
    //
    // 這條是分辨問題性質的關鍵：
    //   通過 → 問題出在「同時動作」，是並行問題
    //   失敗 → 問題出在「同時存在多份文件把手」，與並行無關，
    //          而那會牽連到整個產品（引擎／文字／儲存／列印各持一份把手）
    void fourExtractorsAliveButUsedOneAtATime() {
        std::vector<std::unique_ptr<TextExtractor>> workers;
        for (int i = 0; i < 4; ++i) {
            auto extractor = std::make_unique<TextExtractor>();
            auto opened = std::make_shared<std::atomic<bool>>(false);
            extractor->open(copies_[0], "", [opened](domain::DocumentError error) {
                opened->store(error == domain::DocumentError::None);
            });
            extractor->waitForIdle();
            QVERIFY2(opened->load(), qPrintable(QStringLiteral("第 %1 個擷取器開檔失敗").arg(i)));
            workers.push_back(std::move(extractor));
        }

        for (int i = 0; i < 4; ++i) {
            std::atomic<int> withText{0};
            for (int page = 0; page < 10; ++page) {
                workers[static_cast<std::size_t>(i)]->withTextPage(
                    page, [&withText](const TextPage* textPage) {
                        if (textPage != nullptr && charCount(*textPage) > 0) withText.fetch_add(1);
                    });
            }
            // 下一個擷取器動作前先等這個完全停下來，確保沒有兩條執行緒同時在 PDFium 裡。
            workers[static_cast<std::size_t>(i)]->waitForIdle();
            qInfo("依序使用 / 第 %d 個擷取器: 有文字 %d / 10", i, withText.load());
            QCOMPARE(withText.load(), 10);
        }
    }

    // 列印 vs 檢視：渲染與文字擷取同時動作（ADR-005 的「仍未處理的風險」）。
    //
    // 這是產品裡真的會發生的並行——列印是長時間大量渲染，而使用者在列印時仍然
    // 會捲動；搜尋與建索引也一樣。兩者各自持有把手與執行緒，正是先前被證實
    // 會安靜掉資料的形狀。
    //
    // 加上行程級序列化鎖（engine/pdfium_lock.h）之後，這條必須穩定通過。
    // 拿掉那把鎖，它就會開始間歇性地少頁或少圖磚——而那正是使用者會遇到、
    // 卻沒有任何錯誤訊息的缺陷。
    void renderingAndTextExtractionMayRunAtTheSameTime() {
        constexpr int kTiles = 40;
        constexpr int kTextPages = 40;

        engine::PdfiumEngine renderer;
        auto rendererOpened = std::make_shared<std::atomic<bool>>(false);
        renderer.openDocument(copies_[0], "", [rendererOpened](engine::OpenResult result) {
            rendererOpened->store(result.ok());
        });
        renderer.waitForIdle();
        QVERIFY2(rendererOpened->load(), "渲染引擎開檔失敗");

        TextExtractor extractor;
        auto textOpened = std::make_shared<std::atomic<bool>>(false);
        extractor.open(copies_[0], "", [textOpened](domain::DocumentError error) {
            textOpened->store(error == domain::DocumentError::None);
        });
        extractor.waitForIdle();
        QVERIFY2(textOpened->load(), "文字擷取開檔失敗");

        std::atomic<int> tilesOk{0};
        std::atomic<int> pagesWithText{0};

        // 兩邊同時把工作排進各自的佇列，兩條 PDFium 執行緒因此重疊動作。
        std::thread renderThread([&] {
            for (int i = 0; i < kTiles; ++i) {
                renderer.renderTile(
                    domain::TileKey{i % kPages, domain::exactScaleKey(1.0), 0, 0,
                                    domain::Rotation::None, false},
                    engine::RenderOptions{}, domain::TaskPriority::Visible,
                    engine::CancellationToken{}, [&tilesOk](engine::RenderResult result) {
                        if (result.ok()) tilesOk.fetch_add(1);
                    });
            }
        });
        std::thread textThread([&] {
            for (int i = 0; i < kTextPages; ++i) {
                extractor.withTextPage(i % kPages, [&pagesWithText](const TextPage* page) {
                    if (page != nullptr && charCount(*page) > 0) pagesWithText.fetch_add(1);
                });
            }
        });
        renderThread.join();
        textThread.join();
        renderer.waitForIdle();
        extractor.waitForIdle();

        qInfo("並行：圖磚 %d / %d，有文字的頁 %d / %d", tilesOk.load(), kTiles,
              pagesWithText.load(), kTextPages);
        QCOMPARE(tilesOk.load(), kTiles);
        QCOMPARE(pagesWithText.load(), kTextPages);
    }

    void aLaterExtractorStillOpensCleanly() {
        // 前面兩條跑完之後，行程狀態必須仍然乾淨。
        //
        // 這條的價值在於它是「有沒有人又把並行加回來」的哨兵：並行一旦重新出現，
        // 污染的是行程級狀態，症狀就是這裡開檔失敗（錯誤碼 2 FORMAT），
        // 而不是在真正出錯的那段程式碼上失敗。
        auto extractor = std::make_unique<TextExtractor>();
        auto opened = std::make_shared<std::atomic<int>>(-1);
        extractor->open(copies_[0], "", [opened](domain::DocumentError error) {
            opened->store(static_cast<int>(error));
        });
        extractor->waitForIdle();
        QCOMPARE(opened->load(), static_cast<int>(domain::DocumentError::None));
        QCOMPARE(extractor->pageCount(), kPages);
    }

private:
    static void report(const char* label, int attempt, const Outcome& outcome) {
        qInfo("%s #%d: 有文字 %d / %d，缺 %d，開檔失敗 %d", label, attempt,
              outcome.pagesWithText, kPages, outcome.pagesMissing, outcome.openFailures);
    }

    std::unique_ptr<QTemporaryDir> dir_;
    std::vector<std::string> copies_;
};

QTEST_MAIN(TestPdfiumConcurrency)
#include "test_pdfium_concurrency.moc"
