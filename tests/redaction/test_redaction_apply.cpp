// 塗黑套用（PRD-ANN-032）。
//
// 這組測試的核心只有一句話：**被塗黑的原文必須從檔案裡消失**。
// 只蓋一個黑色矩形上去是業界最常見也最嚴重的 Redaction 事故——底下的文字仍然
// 可以被複製、被搜尋、被 strings 撈出來。PRD 的驗收標準寫的是「以文字擷取驗證
// 原文不可還原」，所以這裡兩條路都驗：文字層讀不到，位元組裡也找不到。

#include <QtTest>

#include <QTemporaryDir>

#include <atomic>

#include "engine/fonts/cjk_font_library.h"
#include "engine/redaction/redaction_applier.h"
#include "engine/objects/pdf_source_document.h"
#include "engine/redaction/redaction_marks.h"
#include "engine/text/text_extractor.h"
#include "redaction_fixture.h"

using namespace alioth;
using namespace alioth::engine::redaction;
using alioth::test::fixture::kFontSize;
using alioth::test::fixture::kImageBottom;
using alioth::test::fixture::kImageLeft;
using alioth::test::fixture::kImageRight;
using alioth::test::fixture::kImageTop;
using alioth::test::fixture::kSecretLineBaseline;
using alioth::test::fixture::kSecretLineLeft;

namespace {

// 蓋住 SECRET-ALPHA 那一行的區域。左右各留一點餘裕，模擬使用者實際框選的樣子。
domain::RectF secretLineArea() {
    return domain::RectF{kSecretLineLeft - 2.0, kSecretLineBaseline - 4.0,
                         kSecretLineLeft + 90.0, kSecretLineBaseline + 14.0};
}

domain::RedactionMark markFor(const domain::RectF& area, int pageIndex = 0) {
    domain::RedactionMark mark;
    mark.pageIndex = pageIndex;
    mark.areas.push_back(area);
    mark.author = "測試者";
    return mark;
}

domain::RedactionPlan planFor(const domain::RectF& area) {
    domain::RedactionMarkSet marks;
    marks.add(markFor(area));
    return domain::RedactionPlan{std::move(marks), domain::IrreversibleConsent::confirmed()};
}

// 用 TextExtractor 讀回整頁文字。這是 PRD 指定的驗證方式——
// 自己解內容串流只會驗到「我們以為自己刪了」，讀不到別人怎麼看這份檔案。
std::string extractPageText(const std::string& bytes, const QString& dir, int pageIndex = 0) {
    const QString path = dir + QStringLiteral("/extract.pdf");
    if (!test::writeBytesTo(path, bytes)) return {};

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

class TestRedactionApply : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void redactedTextIsUnrecoverable() {
        const std::string source =
            test::toStdString(test::makeRedactionFixture());

        // 前提檢查：塗黑之前原文讀得到。少了這一步，「讀不到」可能只是
        // 因為文字層本來就抽不出東西，測試會在什麼都沒驗的情況下變綠。
        const std::string before = extractPageText(source, dir_->path());
        QVERIFY2(before.find(test::fixture::kSecretLine) != std::string::npos,
                 "語料本身讀不到機密字串，測試前提不成立");

        const ApplyResult result = applyRedactions(source, planFor(secretLineArea()));
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.stats.removedStrings > 0);

        // 一、文字擷取讀不到（PRD-ANN-032 的驗收標準）
        const std::string after = extractPageText(result.bytes, dir_->path());
        QVERIFY2(after.find(test::fixture::kSecretLine) == std::string::npos,
                 "文字層仍讀得到被塗黑的字串");

        // 二、位元組層也找不到。文字層讀不到但位元組還在，代表內容只是被移出
        // 顯示路徑而沒有真的刪掉——用 strings 或任何解析器都還撈得回來。
        QVERIFY2(!test::bytesContain(result.bytes,
                                                test::fixture::kSecretLine),
                 "檔案位元組裡仍找得到被塗黑的字串");

        // 沒被標記的內容必須留著，否則塗黑變成刪頁。
        QVERIFY(after.find(test::fixture::kPublicLine) != std::string::npos);
    }

    // 中文覆蓋文字（ADR-007）。
    //
    // 這裡的失敗模式比一般的缺字嚴重：塗黑的覆蓋文字若整段消失，畫面上看起來
    // 就是「一塊乾淨的黑條」——那看起來像塗黑成功了，使用者不會發現標示不見了。
    // 因此驗的是資源真的註冊了，而不只是「產生成功」。
    void cjkOverlayTextEmbedsTheFont() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("這台機器上沒有 CJK 字型，跳過");
        }

        const std::string source = test::toStdString(test::makeRedactionFixture());
        domain::RedactionMark mark = markFor(secretLineArea());
        mark.overlayText = "\xE5\xB7\xB2\xE5\xA1\x97\xE9\xBB\x91";  // 已塗黑
        domain::RedactionMarkSet marks;
        marks.add(mark);
        const ApplyResult result = applyRedactions(
            source, domain::RedactionPlan{std::move(marks),
                                          domain::IrreversibleConsent::confirmed()});
        QVERIFY2(result.ok, result.diagnostic.c_str());

        // 內容串流引用了 CJK 資源名稱……
        QVERIFY2(result.bytes.find("AliothRedactCJK") != std::string::npos,
                 "覆蓋文字沒有切換到 CJK 字型");
        // ……而且真的有一個 Type0 字型被寫進去。只有名稱沒有物件的話，
        // 中文會整段消失，而黑條看起來仍然正常。
        QVERIFY2(result.bytes.find("/Type0") != std::string::npos,
                 "沒有內嵌 Type0 字型，覆蓋文字的中文會整段消失");
        QVERIFY(result.bytes.find("/Identity-H") != std::string::npos);

        // 塗黑本身仍然有效——加了覆蓋文字不能讓原文留下來。
        QVERIFY2(!test::bytesContain(result.bytes, test::fixture::kSecretLine),
                 "加了覆蓋文字之後原文又留在檔案裡了");
    }

    void cjkOverlayFailsLoudlyWhenTheGlyphIsMissing() {
        // 缺字時必須整個失敗。畫一半的覆蓋文字加上黑條，看起來仍然像塗黑成功。
        const std::string source = test::toStdString(test::makeRedactionFixture());
        domain::RedactionMark mark = markFor(secretLineArea());
        // U+E000 是私用區，任何正常字型都不會有這個字。
        mark.overlayText = "\xEE\x80\x80";
        domain::RedactionMarkSet marks;
        marks.add(mark);
        const ApplyResult result = applyRedactions(
            source, domain::RedactionPlan{std::move(marks),
                                          domain::IrreversibleConsent::confirmed()});
        QVERIFY(!result.ok);
        QVERIFY2(!result.diagnostic.empty(), "失敗時必須帶原因（IL-4）");
    }

    void redactionSurvivesCompressedContentStreams() {
        // 實務上的檔案幾乎都是 FlateDecode。解不開就編輯不了，
        // 而「解不開時安靜地什麼都沒做」是最危險的失敗模式。
        test::RedactionFixtureOptions options;
        options.compressContent = true;
        const std::string source =
            test::toStdString(test::makeRedactionFixture(options));

        const ApplyResult result = applyRedactions(source, planFor(secretLineArea()));
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.stats.removedStrings > 0);

        const std::string after = extractPageText(result.bytes, dir_->path());
        QVERIFY(after.find(test::fixture::kSecretLine) == std::string::npos);
        QVERIFY(!test::bytesContain(result.bytes,
                                               test::fixture::kSecretLine));
    }

    void markingAloneLeavesContentIntact() {
        // 兩階段語意：標記可逆，只是註解；沒套用之前原文必須完好。
        // 這條反過來守住上一條——若標記就把內容刪了，兩階段就名存實亡。
        const std::string source =
            test::toStdString(test::makeRedactionFixture());

        domain::RedactionMarkSet marks;
        marks.add(markFor(secretLineArea()));
        const engine::objects::BuildResult marked = markRedactions(source, marks);
        QVERIFY2(marked.ok, marked.diagnostic.c_str());

        const std::string after = extractPageText(marked.bytes, dir_->path());
        QVERIFY2(after.find(test::fixture::kSecretLine) != std::string::npos,
                 "只標記不套用，原文卻消失了");

        // 標記是附加的，所以原檔前綴不變——它跟其他註解一樣不破壞簽章。
        QVERIFY(marked.bytes.size() > source.size());
        QCOMPARE(marked.bytes.compare(0, source.size(), source), 0);
    }

    void applyingMarkedRedactionsRemovesTheMarksToo() {
        const std::string source =
            test::toStdString(test::makeRedactionFixture());

        domain::RedactionMarkSet marks;
        marks.add(markFor(secretLineArea()));
        const engine::objects::BuildResult marked = markRedactions(source, marks);
        QVERIFY(marked.ok);

        const ApplyResult result =
            applyMarkedRedactions(marked.bytes, domain::IrreversibleConsent::confirmed());
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const std::string after = extractPageText(result.bytes, dir_->path());
        QVERIFY(after.find(test::fixture::kSecretLine) == std::string::npos);

        // 標記留著會讓人以為還沒處理，而它此時已經沒有任何作用。
        engine::objects::PdfSourceDocument reopened;
        QVERIFY(reopened.open(result.bytes) == engine::objects::SourceStatus::Ok);
        QVERIFY(readRedactionMarks(reopened).empty());
    }

    void partiallyOverlappingStringIsRemovedWholeByDefault() {
        // 預設策略是過度移除而不是留下殘字：切一半會因為字距與連字而位置錯亂，
        // 而殘字對法務用途等同外洩。
        const std::string source =
            test::toStdString(test::makeRedactionFixture());

        // 只蓋住 SECRET-ALPHA 的前三個字寬。
        const domain::RectF partial{kSecretLineLeft - 2.0, kSecretLineBaseline - 4.0,
                                    kSecretLineLeft + 20.0, kSecretLineBaseline + 14.0};

        const ApplyResult result = applyRedactions(source, planFor(partial));
        QVERIFY2(result.ok, result.diagnostic.c_str());

        const std::string after = extractPageText(result.bytes, dir_->path());
        QVERIFY2(after.find(test::fixture::kSecretLine) == std::string::npos,
                 "部分重疊只移除了一部分，留下了殘字");
    }

    void fullyContainedPolicyLeavesPartialOverlapsAlone() {
        // 非預設策略必須是顯式選擇，而且它的代價要被測試釘住：
        // 部分重疊的字會留在檔案裡。
        const std::string source =
            test::toStdString(test::makeRedactionFixture());

        domain::RedactionMarkSet marks;
        marks.add(markFor(domain::RectF{kSecretLineLeft - 2.0, kSecretLineBaseline - 4.0,
                                        kSecretLineLeft + 20.0, kSecretLineBaseline + 14.0}));
        domain::RedactionPlan plan{std::move(marks), domain::IrreversibleConsent::confirmed()};
        plan.setTextPolicy(domain::PartialOverlapPolicy::RemoveOnlyFullyContained);

        const ApplyResult result = applyRedactions(source, plan);
        QVERIFY(result.ok);

        const std::string after = extractPageText(result.bytes, dir_->path());
        QVERIFY2(after.find(test::fixture::kSecretLine) != std::string::npos,
                 "選了 RemoveOnlyFullyContained 卻仍移除了部分重疊的字串");
    }

    void imageInsideTheAreaIsRemoved() {
        const std::string source =
            test::toStdString(test::makeRedactionFixture());
        QVERIFY(test::bytesContain(source, test::fixture::kImagePixels));

        const domain::RectF area{kImageLeft - 2.0, kImageBottom - 2.0, kImageRight + 2.0,
                                 kImageTop + 2.0};
        const ApplyResult result = applyRedactions(source, planFor(area));
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QVERIFY(result.stats.removedImages + result.stats.removedInlineImages > 0);

        QVERIFY2(!test::bytesContain(result.bytes,
                                               test::fixture::kImagePixels),
                 "影像的像素資料仍留在檔案裡");
    }

    void annotationsInsideTheAreaAreRemoved() {
        // Redaction 最常被忘記的一塊：便利貼的內容不在內容串流裡，
        // 只刪內容串流的話，註解裡的機密原封不動。
        const std::string source =
            test::toStdString(test::makeRedactionFixture());
        QVERIFY(test::bytesContain(source, test::fixture::kRemovedNote));

        // 該則便利貼在語料裡位於影像附近（/Rect [60 195 80 215]），不是機密文字那一行。
        const domain::RectF area{kImageLeft - 2.0, kImageBottom - 6.0, kImageRight + 2.0,
                                 kImageTop + 2.0};
        const ApplyResult result = applyRedactions(source, planFor(area));
        QVERIFY(result.ok);
        QVERIFY(result.stats.removedAnnotations > 0);

        QVERIFY2(!test::bytesContain(result.bytes,
                                               test::fixture::kRemovedNote),
                 "區域內的註解內容仍留在檔案裡");
        QVERIFY2(test::bytesContain(result.bytes,
                                               test::fixture::kKeptNote),
                 "區域外的註解被誤刪");
    }

    void encryptedDocumentIsRejected() {
        // 加密文件的字串與串流要加密後才能寫回。寧可明確拒絕，
        // 也不要寫出一份讀不出來、看起來卻像處理過的檔案。
        test::RedactionFixtureOptions options;
        options.encrypted = true;
        const std::string source =
            test::toStdString(test::makeRedactionFixture(options));

        const ApplyResult result = applyRedactions(source, planFor(secretLineArea()));
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
    }

    void emptyPlanChangesNothing() {
        const std::string source =
            test::toStdString(test::makeRedactionFixture());
        const domain::RedactionPlan empty{domain::RedactionMarkSet{},
                                          domain::IrreversibleConsent::confirmed()};

        const ApplyResult result = applyRedactions(source, empty);
        QVERIFY(result.ok);
        QCOMPARE(result.stats.removedStrings, 0);

        const std::string after = extractPageText(result.bytes, dir_->path());
        QVERIFY(after.find(test::fixture::kSecretLine) != std::string::npos);
    }

    void invalidMarksAreRejectedNotTreatedAsWholePage() {
        // 空的 areas 若被當成「整頁」，一個組錯的標記就會清空整份文件。
        domain::RedactionMark broken;
        broken.pageIndex = 0;
        QVERIFY(!broken.isValid());

        domain::RedactionMarkSet marks;
        marks.add(broken);
        const domain::RedactionPlan plan{std::move(marks),
                                         domain::IrreversibleConsent::confirmed()};

        const std::string source =
            test::toStdString(test::makeRedactionFixture());
        const ApplyResult result = applyRedactions(source, plan);

        // 不論實作選擇拒絕整份計畫或略過該標記，都不得移除任何內容。
        if (result.ok) {
            QCOMPARE(result.stats.removedStrings, 0);
            const std::string after = extractPageText(result.bytes, dir_->path());
            QVERIFY(after.find(test::fixture::kSecretLine) != std::string::npos);
        }
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestRedactionApply)
#include "test_redaction_apply.moc"
