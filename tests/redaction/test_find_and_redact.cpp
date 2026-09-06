// Find and Redact 的標記階段（PRD-ANN-033）。
//
// 這一層先前沒有直接測試。它的責任只有一個：把搜尋命中變成**標記**，
// 而且絕對不套用——誤判在這個功能上是必然的（同一組數字可能是身分證字號、
// 也可能是零件編號），所以「搜尋 → 標記 → 人工檢視 → 套用」的中間那一步
// 不能被跳過。因此本測試除了驗命中，也驗輸入檔在標記後一個位元組都沒變。

#include <QtTest>

#include <QTemporaryDir>

#include "engine/redaction/find_and_redact.h"
#include "redaction_fixture.h"

using namespace alioth;
using alioth::test::fixture::kPublicLine;
using alioth::test::fixture::kSecretLine;
using engine::redaction::FindRedactOptions;

class TestFindAndRedact : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        path_ = dir_->filePath(QStringLiteral("corpus.pdf"));
        original_ = alioth::test::makeRedactionFixture();
        QFile file(path_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(original_);
        file.close();
    }

    void matchesProduceMarksWithNonEmptyAreas() {
        const auto result = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, kSecretLine, {});
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.matchCount >= 1);
        QVERIFY(!result.marks.empty());

        for (const domain::RedactionMark& mark : result.marks.marks()) {
            // 空的 areas 是無效標記，套用端會把它當成什麼都不做，
            // 而使用者會以為已經塗掉了。
            QVERIFY(!mark.areas.empty());
            for (const domain::RectF& area : mark.areas) {
                QVERIFY(area.width() > 0.0);
                QVERIFY(area.height() > 0.0);
            }
        }
    }

    void markedAreaCoversTheMatchedText() {
        // 命中外框要真的蓋住那段字。這裡用語料裡已知的座標驗證，
        // 不是驗「有回傳矩形」而已。
        const auto result = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, kSecretLine, {});
        QVERIFY(result.ok);
        QCOMPARE(result.marks.size(), std::size_t{1});

        const domain::RectF& area = result.marks.marks()[0].areas.front();
        const double expectedLeft = alioth::test::fixture::kSecretLineLeft;
        const double expectedRight =
            expectedLeft + alioth::test::fixture::kGlyphAdvance *
                               static_cast<double>(std::string(kSecretLine).size());

        QVERIFY2(area.left <= expectedLeft + 1.5,
                 qPrintable(QStringLiteral("左緣 %1 沒蓋住 %2").arg(area.left).arg(expectedLeft)));
        QVERIFY2(area.right >= expectedRight - 1.5,
                 qPrintable(QStringLiteral("右緣 %1 沒蓋到 %2").arg(area.right).arg(expectedRight)));
        // 基線上下都要含到，否則字的上伸與下伸部會露出來。
        QVERIFY(area.top > alioth::test::fixture::kSecretLineBaseline);
        QVERIFY(area.bottom < alioth::test::fixture::kSecretLineBaseline);
    }

    void paddingExpandsTheArea() {
        FindRedactOptions tight;
        tight.padding = 0.0;
        FindRedactOptions loose;
        loose.padding = 5.0;

        const auto a = engine::redaction::findAndMarkRedactions(path_.toStdString(), {},
                                                               kSecretLine, tight);
        const auto b = engine::redaction::findAndMarkRedactions(path_.toStdString(), {},
                                                               kSecretLine, loose);
        QVERIFY(a.ok && b.ok);
        const domain::RectF& small = a.marks.marks().front().areas.front();
        const domain::RectF& big = b.marks.marks().front().areas.front();
        QVERIFY(big.width() > small.width());
        QVERIFY(big.height() > small.height());
    }

    void caseSensitivityIsHonoured() {
        FindRedactOptions sensitive;
        sensitive.matchCase = true;
        const auto miss = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, QByteArray(kSecretLine).toLower().toStdString(), sensitive);
        QVERIFY(miss.ok);
        QCOMPARE(miss.matchCount, 0);

        const auto hit = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, QByteArray(kSecretLine).toLower().toStdString(), {});
        QVERIFY(hit.ok);
        QVERIFY(hit.matchCount >= 1);
    }

    void publicTextIsNotMarked() {
        // 只該標到查詢的那段。標多了就是把不該刪的東西刪掉，
        // 而塗黑是不可逆的。
        const auto result = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, kSecretLine, {});
        QVERIFY(result.ok);

        const auto publicResult = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, kPublicLine, {});
        QVERIFY(publicResult.ok);
        QVERIFY(publicResult.matchCount >= 1);

        // 兩者的矩形不該重疊：語料裡兩行相距 50 pt。
        const domain::RectF& secret = result.marks.marks().front().areas.front();
        const domain::RectF& publicArea = publicResult.marks.marks().front().areas.front();
        QVERIFY(secret.bottom > publicArea.top || publicArea.bottom > secret.top);
    }

    void noMatchIsSuccessWithZeroMarks() {
        // 找不到不是錯誤。回報失敗會讓 UI 顯示錯誤對話框，
        // 而使用者只是打錯字。
        const auto result = engine::redaction::findAndMarkRedactions(
            path_.toStdString(), {}, "THIS-STRING-IS-NOT-PRESENT", {});
        QVERIFY(result.ok);
        QCOMPARE(result.matchCount, 0);
        QVERIFY(result.marks.empty());
    }

    void emptyQueryIsRejected() {
        const auto result = engine::redaction::findAndMarkRedactions(path_.toStdString(), {}, "",
                                                                    {});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void missingFileIsReportedNotCrashed() {
        const auto result = engine::redaction::findAndMarkRedactions(
            dir_->filePath(QStringLiteral("nope.pdf")).toStdString(), {}, kSecretLine, {});
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void markCountIsCappedAndReported() {
        FindRedactOptions options;
        options.maxMarks = 1;
        // 語料裡 "SECRET" 出現在多處（Tj 與 TJ 陣列）。
        const auto result =
            engine::redaction::findAndMarkRedactions(path_.toStdString(), {}, "SECRET", options);
        QVERIFY(result.ok);
        QVERIFY(static_cast<int>(result.marks.size()) <= 1);
        // 被截斷這件事必須讓使用者知道，否則他會以為已經標完了。
        QVERIFY(result.truncated);
    }

    void metadataFromOptionsReachesTheMarks() {
        FindRedactOptions options;
        options.author = "Reviewer";
        options.subject = "個資";
        options.note = "身分證字號";
        options.fillColor = domain::ColorRgb{0.2, 0.2, 0.2};

        const auto result = engine::redaction::findAndMarkRedactions(path_.toStdString(), {},
                                                                    kSecretLine, options);
        QVERIFY(result.ok);
        QVERIFY(!result.marks.empty());
        const domain::RedactionMark& mark = result.marks.marks().front();
        QCOMPARE(mark.fillColor.r, 0.2);
    }

    void markingDoesNotModifyTheInputFile() {
        // 這是本階段最重要的性質：標記絕不套用。
        // 若哪天有人把套用「順手」接進來，這條會失敗。
        const auto result = engine::redaction::findAndMarkRedactions(path_.toStdString(), {},
                                                                    kSecretLine, {});
        QVERIFY(result.ok);
        QVERIFY(result.matchCount >= 1);

        QFile file(path_);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray after = file.readAll();
        file.close();
        QCOMPARE(after, original_);
        // 原文當然還在——標記階段本來就不刪東西。
        QVERIFY(after.contains(kSecretLine));
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString path_;
    QByteArray original_;
};

QTEST_MAIN(TestFindAndRedact)
#include "test_find_and_redact.moc"
