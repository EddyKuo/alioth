// Sanitize（PRD-ANN-034）。
//
// 與塗黑同一個判準：清掉的東西必須從**位元組**裡消失，不是從顯示路徑消失。
// 中繼資料尤其容易只被覆蓋而不是刪除——增量附加一份新的 /Info 上去，
// 舊的作者名仍然躺在檔案裡，`strings` 一撈就出來。

#include <QtTest>

#include "engine/redaction/sanitizer.h"
#include "redaction_fixture.h"

using namespace alioth;
using namespace alioth::engine::redaction;

class TestSanitize : public QObject {
    Q_OBJECT

private slots:
    void removesEveryMetadataChannelByDefault() {
        const std::string source = test::toStdString(test::makeRedactionFixture());

        // 前提檢查：這些東西本來都在。少了這一步，「清掉了」可能只是語料裡本來就沒有。
        QVERIFY(test::bytesContain(source, test::fixture::kInfoAuthor));
        QVERIFY(test::bytesContain(source, test::fixture::kInfoProducer));
        QVERIFY(test::bytesContain(source, test::fixture::kXmpValue));
        QVERIFY(test::bytesContain(source, test::fixture::kPieceInfo));
        QVERIFY(test::bytesContain(source, test::fixture::kEmbeddedFile));
        QVERIFY(test::bytesContain(source, test::fixture::kJavaScript));

        const SanitizeResult result = sanitizeDocument(source);
        QVERIFY2(result.ok, result.diagnostic.c_str());

        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kInfoAuthor),
                 "/Info 的作者仍在位元組裡");
        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kInfoProducer),
                 "/Info 的製作程式仍在位元組裡");
        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kXmpValue),
                 "XMP 中繼資料仍在位元組裡");
        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kPieceInfo),
                 "/PieceInfo 仍在位元組裡");
        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kEmbeddedFile),
                 "嵌入檔案仍在位元組裡");
        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kJavaScript),
                 "嵌入的 JavaScript 仍在位元組裡");

        QVERIFY(result.stats.clearedInfoKeys > 0);
        QVERIFY(result.stats.removedMetadataStreams > 0);
    }

    void pageContentIsNotTouched() {
        // Sanitize 清的是中繼資料，不是內容。把頁面文字也清掉是另一種災難。
        const std::string source = test::toStdString(test::makeRedactionFixture());
        const SanitizeResult result = sanitizeDocument(source);
        QVERIFY(result.ok);

        QVERIFY(test::bytesContain(result.bytes, test::fixture::kSecretLine));
        QVERIFY(test::bytesContain(result.bytes, test::fixture::kPublicLine));
    }

    void optionsAreHonouredIndividually() {
        // 每個開關都要真的獨立。全開全關都通過、但單開一個沒作用的實作，
        // 只有逐項驗才抓得到。
        const std::string source = test::toStdString(test::makeRedactionFixture());

        SanitizeOptions onlyInfo;
        onlyInfo.clearXmpMetadata = false;
        onlyInfo.clearPieceInfo = false;
        onlyInfo.removeEmbeddedFiles = false;
        onlyInfo.removeJavaScript = false;

        const SanitizeResult result = sanitizeDocument(source, onlyInfo);
        QVERIFY(result.ok);
        QVERIFY(!test::bytesContain(result.bytes, test::fixture::kInfoAuthor));
        QVERIFY2(test::bytesContain(result.bytes, test::fixture::kXmpValue),
                 "只要求清 /Info，XMP 卻也被清掉了");
    }

    void documentWithoutMetadataSucceedsWithZeroStats() {
        // 沒有中繼資料的文件不該被當成錯誤——「沒東西可清」是正常結果。
        test::RedactionFixtureOptions options;
        options.withEmbeddedFile = false;
        options.withJavaScript = false;
        const std::string source = test::toStdString(test::makeRedactionFixture(options));

        const SanitizeResult result = sanitizeDocument(source);
        QVERIFY(result.ok);
        QCOMPARE(result.stats.removedEmbeddedFiles, 0);
        QCOMPARE(result.stats.removedJavaScript, 0);
    }

    void encryptedDocumentIsRejected() {
        test::RedactionFixtureOptions options;
        options.encrypted = true;
        const std::string source = test::toStdString(test::makeRedactionFixture(options));

        const SanitizeResult result = sanitizeDocument(source);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void sanitizeRewritesInsteadOfAppending() {
        // 增量附加會讓舊的 /Info 留在檔案裡。這條測試釘住「必須全檔重寫」這個決定：
        // 若哪天有人為了保簽章把它改成附加，這裡會紅。
        const std::string source = test::toStdString(test::makeRedactionFixture());
        const SanitizeResult result = sanitizeDocument(source);
        QVERIFY(result.ok);

        const bool prefixPreserved =
            result.bytes.size() >= source.size() &&
            result.bytes.compare(0, source.size(), source) == 0;
        QVERIFY2(!prefixPreserved,
                 "輸出保留了原檔前綴，代表走的是增量附加——舊的中繼資料還在裡面");
    }
};

QTEST_APPLESS_MAIN(TestSanitize)
#include "test_sanitize.moc"
