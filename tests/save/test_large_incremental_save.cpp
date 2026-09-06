// 100 MB 文件的增量儲存量測（PRD-IO-001 的兩條驗收數字）。
//
// PRD §8.1 要求：100 MB 文件加一個註解 ≤ 300 毫秒、增量段 ≤ 20 KB。
// 這兩個數字先前一直列在「未量測」，理由是「需要 100 MB 真實文件」——
// 但真實語料不是必要條件：要量的是**寫入路徑**（讀檔 → 附加 → 原子更名），
// 而那條路徑對 100 MB 的內容串流填充與 100 MB 的真實內容沒有差別。
//
// 兩條斷言的性質不同：
//
//   增量段大小是**行為**，與機器無關，因此是硬門檻。超過 20 KB 代表寫入
//   路徑退化成「重寫一大段」，那會直接毀掉「不破壞既有簽章」的賣點。
//
//   耗時與磁碟速度強相關。在 CI 上寫死 300 毫秒只會得到偶發紅燈，所以這裡
//   斷言的是一個寬鬆的上限，並把實測值 qInfo 出來——真正的驗收要在目標
//   機器上看那個數字，而不是看這支測試綠不綠。

#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "app/annotation_service.h"
#include "save_fixture.h"

using alioth::app::AnnotationRequest;
using alioth::app::AnnotationService;
using alioth::domain::Annotation;
using alioth::domain::ColorRgb;
using alioth::domain::RectF;
using alioth::domain::TextNoteGeometry;

namespace {

// PRD §8.1 的兩個數字。
constexpr qint64 kMaxAppendedBytes = 20 * 1024;
constexpr qint64 kTargetMs = 300;

// CI 與開發機的磁碟差異很大，實測值以 qInfo 輸出供人判讀；這裡只擋住
// 「慢了一個數量級」那種真正的退化。
constexpr qint64 kSanityCeilingMs = 10 * kTargetMs;

}  // namespace

class TestLargeIncrementalSave : public QObject {
    Q_OBJECT

private slots:
    void addingOneAnnotationTo100MbStaysWithinBudget();
};

void TestLargeIncrementalSave::addingOneAnnotationTo100MbStaysWithinBudget() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 每頁 1 MB × 100 頁 ≈ 100 MB。用內容串流填充而不是真實內容：
    // 要量的是寫入路徑，而那條路徑對兩者沒有差別。
    const QByteArray bytes = alioth::test::makeBulkyPdf(100, 1024 * 1024);
    QVERIFY2(bytes.size() > 100 * 1024 * 1024,
             qPrintable(QStringLiteral("語料只有 %1 位元組，不足 100 MB").arg(bytes.size())));

    const QString path = dir.filePath(QStringLiteral("large.pdf"));
    QVERIFY(alioth::test::writePdfTo(path, bytes));
    const qint64 originalSize = QFileInfo(path).size();

    Annotation annotation;
    annotation.rect = RectF{40, 300, 60, 320};
    annotation.geometry = TextNoteGeometry{};
    annotation.color = ColorRgb{1.0, 0.85, 0.0};

    AnnotationRequest request;
    request.path = path;
    request.pageIndex = 0;
    request.annotation =
        AnnotationService::stamped(std::move(annotation), QStringLiteral("bench"));

    AnnotationService service;
    QElapsedTimer timer;
    timer.start();
    const auto result = service.addAnnotation(request);
    const qint64 elapsed = timer.elapsed();

    QVERIFY2(result.ok, qPrintable(result.message));

    const qint64 appended = QFileInfo(path).size() - originalSize;
    qInfo() << "100MB incremental save:" << elapsed << "ms, appended" << appended << "bytes";

    // 硬門檻：增量段大小是行為，與機器無關。超過代表寫入路徑退化成
    // 「重寫一大段」，而那會毀掉「不破壞既有簽章」的整個基礎。
    QVERIFY2(appended > 0, "檔案沒有變長——註解根本沒寫進去");
    QVERIFY2(appended <= kMaxAppendedBytes,
             qPrintable(QStringLiteral("增量段 %1 位元組，超過 %2 位元組的上限")
                            .arg(appended)
                            .arg(kMaxAppendedBytes)));

    // 原檔前綴逐位元組不變——這才是「增量」的定義，光看檔案變長不夠：
    // 一個把整份重寫再附加的實作也會讓檔案變長。
    QFile after(path);
    QVERIFY(after.open(QIODevice::ReadOnly));
    QCOMPARE(after.read(originalSize), bytes);
    after.close();

    // 軟門檻：耗時與磁碟速度強相關，寫死 300 毫秒在 CI 上只會偶發紅燈。
    // 目標值仍然記在這裡，實測值由上面的 qInfo 輸出。
    if (elapsed > kTargetMs) {
        qWarning() << "超過 PRD §8.1 的 300 毫秒目標（實測" << elapsed
                   << "ms）——若在目標機器上重現，屬於效能缺陷";
    }
    QVERIFY2(elapsed <= kSanityCeilingMs,
             qPrintable(QStringLiteral("耗時 %1 ms，慢了一個數量級").arg(elapsed)));
}

QTEST_MAIN(TestLargeIncrementalSave)
#include "test_large_incremental_save.moc"
