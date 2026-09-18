// Bates 編號與戳記寫進文件（PRD-PAGE-004 的後半）。
//
// 列印路徑早就能把這些畫在紙上，但法務流程需要的是「拿到檔案的人也看得到編號」。
// 這裡最關鍵的性質是**純附加**：蓋章不該讓既有簽章從「簽署後有變更」變成「無效」，
// 那是本產品與多數工具的差別所在。

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <atomic>

#include "app/stamp_service.h"
#include "engine/text/text_extractor.h"
#include "qa/qpdf_check.h"

using namespace alioth;

namespace {

QByteArray makeThreePagePdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>");
    for (int i = 0; i < 3; ++i) {
        objects.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> >>");
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

std::string pageText(const QString& path, int pageIndex) {
    engine::text::TextExtractor extractor;
    std::atomic<bool> opened{false};
    extractor.open(path.toStdString(), "",
                   [&opened](domain::DocumentError e) { opened = e == domain::DocumentError::None; });
    extractor.waitForIdle();
    if (!opened.load()) return {};

    std::string text;
    extractor.withTextPage(pageIndex, [&text](const engine::text::TextPage* page) {
        if (!page) return;
        text = engine::text::textForRange(*page, engine::text::pageRange(*page));
    });
    extractor.waitForIdle();
    return text;
}

}  // namespace

class TestStampService : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("stamp.pdf"));
        original_ = makeThreePagePdf();

        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(original_);
        file.close();
    }

    void batesNumbersAreWrittenIntoEveryPage() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("<<Bates>>");
        request.useBates = true;
        request.bates.enabled = true;
        request.bates.prefix = QStringLiteral("ALT-");
        request.bates.startNumber = 100;
        request.bates.digits = 5;

        app::StampService service;
        const app::StampResult result = service.applyStamps(request);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.stampedPages, 3);
        QCOMPARE(result.batesNumbers.size(), std::size_t{3});
        QCOMPARE(result.batesNumbers[0], QStringLiteral("ALT-00100"));
        QCOMPARE(result.batesNumbers[2], QStringLiteral("ALT-00102"));

        // 由文字層讀回來，證明它真的在頁面上而不是只在我們的資料結構裡。
        QVERIFY2(pageText(path_, 0).find("ALT-00100") != std::string::npos,
                 "第 1 頁讀不到 Bates 編號");
        QVERIFY(pageText(path_, 2).find("ALT-00102") != std::string::npos);
    }

    void writingIsPureAppendSoSignaturesSurvive() {
        // 這是本功能與「重新產生一份檔案」的關鍵差別。
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("Page <<Page>> of <<Pages>>");

        app::StampService service;
        const app::StampResult result = service.applyStamps(request);
        QVERIFY2(result.ok, qPrintable(result.message));

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray after = file.readAll();
        file.close();

        QVERIFY(after.size() > original_.size());
        QCOMPARE(after.left(original_.size()), original_);
        QCOMPARE(result.previousSize, static_cast<quint64>(original_.size()));
    }

    void tokensAreExpandedPerPage() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("Page <<Page>> of <<Pages>>");

        app::StampService service;
        QVERIFY(service.applyStamps(request).ok);

        QVERIFY(pageText(path_, 0).find("Page 1 of 3") != std::string::npos);
        QVERIFY(pageText(path_, 1).find("Page 2 of 3") != std::string::npos);
    }

    void onlySelectedPagesAreStamped() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("MARKED");
        request.pages = {1};

        app::StampService service;
        const app::StampResult result = service.applyStamps(request);
        QVERIFY(result.ok);
        QCOMPARE(result.stampedPages, 1);

        QVERIFY(pageText(path_, 0).find("MARKED") == std::string::npos);
        QVERIFY(pageText(path_, 1).find("MARKED") != std::string::npos);
    }

    void duplicateBatesNumbersAreRejectedBeforeWriting() {
        // increment 為 0 時整批是同一個號碼，在法務用途上等同沒有編號。
        // 必須在寫檔前擋下——蓋到一半才發現時檔案已經被改了。
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("<<Bates>>");
        request.useBates = true;
        request.bates.enabled = true;
        request.bates.increment = 0;

        app::StampService service;
        const app::StampResult result = service.applyStamps(request);
        QVERIFY(!result.ok);
        QVERIFY(!result.message.isEmpty());

        // 被擋下時檔案不得有任何變動。
        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original_);
    }

    void cjkTextIsStampedWithAnEmbeddedSubset() {
        // ADR-007：CJK 走內嵌的思源黑體子集。先前這條路徑一律拒絕非 ASCII，
        // 對一個以繁體中文為主要語系的產品來說等於浮水印功能不能用。
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("機密文件 CONFIDENTIAL");

        app::StampService service;
        const app::StampResult result = service.applyStamps(request);
        QVERIFY2(result.ok, qPrintable(result.message));

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray after = file.readAll();
        file.close();

        // 純附加：原檔前綴一個位元組都不能動，否則既有簽章會失效。
        QCOMPARE(after.left(original_.size()), original_);

        // 字型必須真的被內嵌並登記。少了任何一半，檔案本身合法而畫面上是空白——
        // 空白的浮水印看起來像功能沒作用，使用者查不出原因。
        QVERIFY2(after.contains("AliothStampCJK"), "CJK 字型資源沒有登記到頁面");
        QVERIFY2(after.contains("/Subtype /CIDFontType2") ||
                     after.contains("/Subtype/CIDFontType2"),
                 "沒有內嵌 CIDFontType2 字型");
        // 拉丁段仍走標準 14，不會被塞進 CJK 字型裡當雙位元組讀。
        QVERIFY(after.contains("AliothStampF0"));
    }

    void emptyTemplateIsRejected() {
        app::StampRequest request;
        request.path = path_;

        app::StampService service;
        QVERIFY(!service.applyStamps(request).ok);
    }

    void outputPassesQpdfStructureCheck() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("CHECKED");

        app::StampService service;
        QVERIFY(service.applyStamps(request).ok);

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path_);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

    // 移除所有頁面標記（PDF-XChange 的 Remove All）。
    //
    // 蓋一份審閱用的浮水印、審完再拿掉，是這個功能唯一的用途。
    // 驗收是「文字層讀不到了」而不是「檔案變小了」——純附加的移除不會讓
    // 檔案變小，它只是把 /Contents 裡的參照摘掉。
    void removingStampsTakesThemOutOfTheTextLayer() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("DRAFT COPY");

        app::StampService service;
        QVERIFY(service.applyStamps(request).ok);
        QVERIFY(pageText(path_, 0).find("DRAFT COPY") != std::string::npos);

        const app::StampResult removed = service.removeStamps(path_);
        QVERIFY2(removed.ok, qPrintable(removed.message));
        QCOMPARE(removed.stampedPages, 3);

        for (int page = 0; page < 3; ++page) {
            QVERIFY2(pageText(path_, page).find("DRAFT COPY") == std::string::npos,
                     "移除之後文字層仍然讀得到戳記");
        }
    }

    // 移除本身也必須是純附加，否則「蓋章不讓簽章失效」這個賣點在
    // 「蓋了又拿掉」的流程上就不成立了。
    void removingIsAlsoPureAppend() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("DRAFT COPY");

        app::StampService service;
        QVERIFY(service.applyStamps(request).ok);

        QFile stamped(path_);
        QVERIFY(stamped.open(QIODevice::ReadOnly));
        const QByteArray afterStamp = stamped.readAll();
        stamped.close();

        QVERIFY(service.removeStamps(path_).ok);

        QFile cleaned(path_);
        QVERIFY(cleaned.open(QIODevice::ReadOnly));
        const QByteArray afterRemove = cleaned.readAll();
        cleaned.close();

        // 前綴逐位元組不變——連第一次蓋章寫進去的那一段也還在。
        QVERIFY(afterRemove.size() > afterStamp.size());
        QCOMPARE(afterRemove.left(afterStamp.size()), afterStamp);
        // 原檔那一段當然也還在。
        QCOMPARE(afterRemove.left(original_.size()), original_);
    }

    // 沒有我們加過的標記時不寫檔。空的附加段只會讓簽章狀態從「有效」變成
    // 「簽署後有變更」，而使用者什麼都沒得到。
    void removingNothingDoesNotTouchTheFile() {
        app::StampService service;
        const app::StampResult removed = service.removeStamps(path_);
        QVERIFY(!removed.ok);
        QVERIFY(!removed.message.isEmpty());

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original_);
    }

    // 移除之後再蓋一次要正常運作：使用者會換一個浮水印再蓋。
    void stampingAgainAfterRemovalWorks() {
        app::StampService service;

        app::StampRequest first;
        first.path = path_;
        first.textTemplate = QStringLiteral("FIRST MARK");
        QVERIFY(service.applyStamps(first).ok);
        QVERIFY(service.removeStamps(path_).ok);

        app::StampRequest second;
        second.path = path_;
        second.textTemplate = QStringLiteral("SECOND MARK");
        QVERIFY(service.applyStamps(second).ok);

        const std::string text = pageText(path_, 0);
        QVERIFY2(text.find("SECOND MARK") != std::string::npos, "第二次蓋章沒有生效");
        QVERIFY2(text.find("FIRST MARK") == std::string::npos, "被移除的戳記又回來了");
    }

    void removedDocumentIsStillStructurallyClean() {
        app::StampRequest request;
        request.path = path_;
        request.textTemplate = QStringLiteral("DRAFT COPY");

        app::StampService service;
        QVERIFY(service.applyStamps(request).ok);
        QVERIFY(service.removeStamps(path_).ok);

        const alioth::test::QpdfCheckResult check = alioth::test::runQpdfCheck(path_);
        if (check.status == alioth::test::QpdfStatus::NotAvailable) {
            QSKIP("qpdf 不在可用位置，略過結構檢查");
        }
        QVERIFY2(check.clean(), qPrintable(check.output));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
    QByteArray original_;
};

QTEST_MAIN(TestStampService)
#include "test_stamp_service.moc"
