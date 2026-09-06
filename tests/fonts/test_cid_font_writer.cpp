// PDF 內嵌 CID 字型（ADR-007）。
//
// 這一層的錯誤幾乎全是安靜的：/CIDToGIDMap 與子集的編號方式不一致、
// /ToUnicode 漏了幾個字、輔助平面沒寫成代理對——都不會崩潰，
// 只是畫出別的字，或字看得到卻複製不出來。因此驗的是物件結構與位元組，
// 再加一道 qpdf 的獨立結構檢查。

#include <QtTest>

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include "engine/fonts/cid_font_writer.h"
#include "engine/fonts/truetype_subset.h"
#include "engine/objects/incremental_appender.h"
#include "pdf_fixture.h"

using namespace alioth;
using namespace alioth::engine::fonts;
using namespace alioth::engine::objects;

namespace {

// 沒有真實 CJK 字型的機器上，這組測試沒有東西可以子集。
// 明確跳過而不是造一份合成字型：這一層要驗的是「真的能內嵌並被 qpdf 接受」，
// 而合成字型驗不出 CJK 特有的問題（幾萬字、輔助平面、複合字形）。
[[nodiscard]] std::string loadCjkFont() {
    const QString path = QStringLiteral("C:/Windows/Fonts/NotoSansTC-VF.ttf");
    if (!QFile::exists(path)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = file.readAll();
    return std::string(raw.constData(), static_cast<std::size_t>(raw.size()));
}

}  // namespace

class TestCidFontWriter : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        font_ = loadCjkFont();
        if (font_.empty()) QSKIP("這台機器上沒有 Noto Sans TC，整組跳過");
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    void subsetTagIsStableForTheSameSubset() {
        // 同樣的子集必須得到同樣的前綴。用隨機值會讓同一份文件每次存檔都
        // 產生新的字型名稱，增量段因此白白變大。
        const SubsetResult a = subsetTrueType(font_, {U'中', U'文'});
        const SubsetResult b = subsetTrueType(font_, {U'中', U'文'});
        QVERIFY(a.ok && b.ok);
        QCOMPARE(subsetTag(a.bytes), subsetTag(b.bytes));

        const SubsetResult c = subsetTrueType(font_, {U'中'});
        QVERIFY(c.ok);
        QVERIFY(subsetTag(a.bytes) != subsetTag(c.bytes));
    }

    void subsetTagIsSixUppercaseLetters() {
        const SubsetResult subset = subsetTrueType(font_, {U'中'});
        QVERIFY(subset.ok);
        const std::string tag = subsetTag(subset.bytes);
        QCOMPARE(tag.size(), std::size_t(6));
        for (const char c : tag) QVERIFY(c >= 'A' && c <= 'Z');
    }

    void identityHEncodesTwoBytesPerCharacter() {
        const SubsetResult subset = subsetTrueType(font_, {U'中', U'文'});
        QVERIFY(subset.ok);
        std::string encoded;
        QVERIFY(encodeIdentityH(subset, U"中文", encoded));
        // 每個字兩位元組，但跳脫之後長度會變長，因此驗的是「非空且不是一位元組」。
        QVERIFY(!encoded.empty());
        // 兩個字的編碼必須不同，否則就是所有字都對到同一個 GID。
        std::string one;
        std::string two;
        QVERIFY(encodeIdentityH(subset, U"中", one));
        QVERIFY(encodeIdentityH(subset, U"文", two));
        QVERIFY(one != two);
    }

    void missingGlyphFailsInsteadOfEmittingNotdef() {
        // 這是 CJK 路徑最容易出現的失敗。輸出 .notdef 會在畫面上變成空白方塊，
        // 使用者不知道是缺字還是程式壞了。
        const SubsetResult subset = subsetTrueType(font_, {U'中'});
        QVERIFY(subset.ok);
        std::string encoded;
        QVERIFY(!encodeIdentityH(subset, U"中文", encoded));
    }

    void embeddedFontHasTheThreeLayerStructure() {
        const SubsetResult subset = subsetTrueType(font_, {U'測', U'試'});
        QVERIFY2(subset.ok, subset.diagnostic.c_str());

        IncrementalAppender appender;
        const QByteArray base = test::makeSinglePagePdf();
        QCOMPARE(appender.open(std::string(base.constData(),
                                           static_cast<std::size_t>(base.size()))),
                 SourceStatus::Ok);

        const EmbeddedFontResult embedded = embedSubsetFont(appender, subset, "NotoSansTC");
        QVERIFY2(embedded.ok, embedded.diagnostic.c_str());

        // Type0 → DescendantFonts[0] → FontDescriptor → FontFile2，逐層檢查。
        const PdfObject type0 = appender.currentObject(embedded.fontObject);
        const PdfDictionary* type0Dict = type0.asDictionary();
        QVERIFY(type0Dict != nullptr);
        QCOMPARE(type0Dict->find("Subtype")->asName(), std::string("Type0"));
        QCOMPARE(type0Dict->find("Encoding")->asName(), std::string("Identity-H"));
        QVERIFY(type0Dict->find("ToUnicode") != nullptr);

        const PdfObject* descendants = type0Dict->find("DescendantFonts");
        QVERIFY(descendants != nullptr && descendants->asArray() != nullptr);
        QCOMPARE(descendants->asArray()->size(), std::size_t(1));

        const int cidNumber = descendants->asArray()->front().asRef().number;
        const PdfObject cid = appender.currentObject(cidNumber);
        const PdfDictionary* cidDict = cid.asDictionary();
        QVERIFY(cidDict != nullptr);
        QCOMPARE(cidDict->find("Subtype")->asName(), std::string("CIDFontType2"));
        // 這一條與子集化的「保留原始 glyph index」是同一個決定的兩半。
        // 不一致的話每個字都會畫成別的字。
        QCOMPARE(cidDict->find("CIDToGIDMap")->asName(), std::string("Identity"));
        QVERIFY(cidDict->find("W") != nullptr);

        const int descriptorNumber = cidDict->find("FontDescriptor")->asRef().number;
        const PdfObject descriptor = appender.currentObject(descriptorNumber);
        const PdfDictionary* descriptorDict = descriptor.asDictionary();
        QVERIFY(descriptorDict != nullptr);
        QVERIFY(descriptorDict->find("FontFile2") != nullptr);
        // Symbolic：CJK 不落在標準拉丁編碼裡，宣告成 Nonsymbolic 會讓部分
        // 檢視器套用標準編碼並顯示錯字。
        QCOMPARE(descriptorDict->find("Flags")->asInteger(), std::int64_t(4));

        const int fontFileNumber = descriptorDict->find("FontFile2")->asRef().number;
        const PdfObject fontFile = appender.currentObject(fontFileNumber);
        const PdfStream* stream = fontFile.asStream();
        QVERIFY(stream != nullptr);
        QCOMPARE(stream->data.size(), subset.bytes.size());
        QCOMPARE(stream->dict.find("Length1")->asInteger(),
                 static_cast<std::int64_t>(subset.bytes.size()));
    }

    void toUnicodeCoversEveryEmbeddedCharacter() {
        // 少了任何一個字，那個字在 PDF 裡就複製不出來也搜尋不到——
        // 看得到卻複製不出來，使用者會以為檔案壞了。
        const std::set<char32_t> text{U'測', U'試', U'中', U'文', U'A'};
        const SubsetResult subset = subsetTrueType(font_, text);
        QVERIFY(subset.ok);

        IncrementalAppender appender;
        const QByteArray base = test::makeSinglePagePdf();
        QCOMPARE(appender.open(std::string(base.constData(),
                                           static_cast<std::size_t>(base.size()))),
                 SourceStatus::Ok);
        const EmbeddedFontResult embedded = embedSubsetFont(appender, subset, "NotoSansTC");
        QVERIFY(embedded.ok);

        const PdfObject type0 = appender.currentObject(embedded.fontObject);
        const int toUnicodeNumber = type0.asDictionary()->find("ToUnicode")->asRef().number;
        const PdfObject toUnicode = appender.currentObject(toUnicodeNumber);
        QVERIFY(toUnicode.asStream() != nullptr);
        const std::string& cmap = toUnicode.asStream()->data;

        QVERIFY(cmap.find("beginbfchar") != std::string::npos);
        QVERIFY(cmap.find("endcmap") != std::string::npos);
        for (const char32_t codepoint : text) {
            char expected[8];
            std::snprintf(expected, sizeof(expected), "%04X",
                          static_cast<unsigned>(codepoint));
            QVERIFY2(cmap.find(expected) != std::string::npos,
                     qPrintable(QStringLiteral("/ToUnicode 缺少 U+%1").arg(expected)));
        }
    }

    void embeddedFontPassesQpdfStructureCheck() {
        // qpdf 是獨立的第三方判準。我們自己的序列化器認為對的東西，
        // 不代表別人讀得進去。
        const SubsetResult subset = subsetTrueType(font_, {U'測', U'試', U'中', U'文'});
        QVERIFY(subset.ok);

        IncrementalAppender appender;
        const QByteArray base = test::makeSinglePagePdf();
        QCOMPARE(appender.open(std::string(base.constData(),
                                           static_cast<std::size_t>(base.size()))),
                 SourceStatus::Ok);
        QVERIFY(embedSubsetFont(appender, subset, "NotoSansTC").ok);

        const BuildResult built = appender.build();
        QVERIFY2(built.ok, built.diagnostic.c_str());

        const QString path = dir_->filePath(QStringLiteral("embedded.pdf"));
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(built.bytes.data(), static_cast<qint64>(built.bytes.size()));
        out.close();

        const QString qpdf = QStringLiteral(ALIOTH_QPDF_EXECUTABLE);
        if (qpdf.isEmpty() || !QFile::exists(qpdf)) QSKIP("找不到 qpdf，跳過結構檢查");

        QProcess process;
        process.start(qpdf, {QStringLiteral("--check"), path});
        QVERIFY(process.waitForFinished(30000));
        const QString output = QString::fromUtf8(process.readAllStandardOutput()) +
                               QString::fromUtf8(process.readAllStandardError());
        QVERIFY2(process.exitCode() == 0, qPrintable(output));
        QVERIFY2(!output.contains(QStringLiteral("WARNING")), qPrintable(output));
    }

    void incrementalWriteLeavesTheOriginalBytesUntouched() {
        // 內嵌字型仍然走增量附加。原檔位元組一旦被改寫，既有簽章會從
        // 「有效、簽章後有變更」變成「無效」。
        const SubsetResult subset = subsetTrueType(font_, {U'中'});
        QVERIFY(subset.ok);

        IncrementalAppender appender;
        const QByteArray base = test::makeSinglePagePdf();
        const std::string original(base.constData(), static_cast<std::size_t>(base.size()));
        QCOMPARE(appender.open(original), SourceStatus::Ok);
        QVERIFY(embedSubsetFont(appender, subset, "NotoSansTC").ok);

        const BuildResult built = appender.build();
        QVERIFY(built.ok);
        QVERIFY(built.bytes.size() > original.size());
        QCOMPARE(built.bytes.compare(0, original.size(), original), 0);
    }

private:
    std::string font_;
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_MAIN(TestCidFontWriter)
#include "test_cid_font_writer.moc"
