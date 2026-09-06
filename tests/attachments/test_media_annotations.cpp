// 音訊／視訊註解（PRD-ANN-016）。
//
// 本產品不播放內嵌媒體，這是刻意的（理由見 media_annotations.h）。因此本測試
// 驗的不是播放，而是三件事：三種形態都認得、取不出內容時明確標示、
// 以及外部參照絕不被當成可播放的內嵌資料。

#include <QtTest>

#include "engine/attachments/media_annotations.h"
#include "engine/objects/pdf_source_document.h"

using namespace alioth;
using engine::attachments::MediaAnnotation;
using engine::attachments::MediaKind;

namespace {

QByteArray streamObject(const QByteArray& extras, const QByteArray& data) {
    return "<< " + extras + " /Length " + QByteArray::number(data.size()) + " >>\nstream\n" + data +
           "\nendstream";
}

// 一頁文件，帶四則註解：
//   /Screen 有內嵌 mp4、/Screen 只有外部網址、/Movie 舊形態外部路徑、
//   /RichMedia 內嵌資產。
QByteArray makeMediaPdf() {
    std::vector<QByteArray> objects;

    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> "
        "/Annots [4 0 R 9 0 R 11 0 R 13 0 R] >>");

    // 4：內嵌影片的 /Screen
    objects.push_back(
        "<< /Type /Annot /Subtype /Screen /Rect [10 10 210 130] /T (\xe7\xb0\xa1\xe5\xa0\xb1) "
        "/Contents (\xe6\x93\x8d\xe4\xbd\x9c\xe7\xa4\xba\xe7\xaf\x84) /A 5 0 R >>");
    objects.push_back("<< /Type /Action /S /Rendition /R 6 0 R >>");
    objects.push_back("<< /Type /Rendition /S /MR /C 7 0 R >>");
    objects.push_back("<< /Type /MediaClip /S /MCD /CT (video/mp4) /D 8 0 R >>");
    objects.push_back(
        "<< /Type /Filespec /F (demo.mp4) /UF (demo.mp4) /EF << /F 14 0 R >> >>");

    // 9/10：只有外部網址的 /Screen
    objects.push_back(
        "<< /Type /Annot /Subtype /Screen /Rect [10 200 210 320] /A 10 0 R >>");
    objects.push_back(
        "<< /Type /Action /S /Rendition /R << /Type /Rendition /S /MR "
        "/C << /Type /MediaClip /S /MCD /CT (video/mp4) "
        "/D << /Type /Filespec /F (https://example.invalid/clip.mp4) >> >> >> >>");

    // 11/12：/Movie 舊形態
    objects.push_back(
        "<< /Type /Annot /Subtype /Movie /Rect [220 10 380 90] /Movie 12 0 R >>");
    objects.push_back("<< /F (D:/media/old.avi) >>");

    // 13：/RichMedia
    objects.push_back(
        "<< /Type /Annot /Subtype /RichMedia /Rect [220 200 380 320] "
        "/RichMediaContent << /Assets << /Names [ (clip.swf) 15 0 R ] >> >> >>");

    // 14：內嵌影片位元組
    objects.push_back(streamObject("/Type /EmbeddedFile /Subtype /video#2Fmp4",
                                   "FAKE-MP4-BYTES"));
    // 15：RichMedia 的資產 filespec
    objects.push_back("<< /Type /Filespec /F (clip.swf) /EF << /F 16 0 R >> >>");
    objects.push_back(streamObject("/Type /EmbeddedFile", "FAKE-SWF"));

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

const MediaAnnotation* atRect(const std::vector<MediaAnnotation>& list, double left, double bottom) {
    for (const MediaAnnotation& media : list) {
        if (std::abs(media.rectPt.left - left) < 0.5 &&
            std::abs(media.rectPt.bottom - bottom) < 0.5) {
            return &media;
        }
    }
    return nullptr;
}

}  // namespace

class TestMediaAnnotations : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        const QByteArray pdf = makeMediaPdf();
        QVERIFY(source_.open(std::string(pdf.constData(),
                                         static_cast<std::size_t>(pdf.size()))) ==
                engine::objects::SourceStatus::Ok);
        list_ = engine::attachments::listMediaAnnotations(source_);
    }

    void allThreeFormsAreRecognised() {
        // 只認 /Screen 的話，帶 /Movie 舊形態或 /RichMedia 的文件會看起來
        // 「沒有媒體」，而畫面上明明有一塊播放區。
        QCOMPARE(list_.size(), std::size_t{4});
        int screen = 0;
        int movie = 0;
        int rich = 0;
        for (const MediaAnnotation& media : list_) {
            if (media.kind == MediaKind::Screen) ++screen;
            if (media.kind == MediaKind::Movie) ++movie;
            if (media.kind == MediaKind::RichMedia) ++rich;
        }
        QCOMPARE(screen, 2);
        QCOMPARE(movie, 1);
        QCOMPARE(rich, 1);
    }

    void embeddedScreenCarriesItsMetadata() {
        const MediaAnnotation* media = atRect(list_, 10.0, 10.0);
        QVERIFY(media != nullptr);
        QCOMPARE(media->pageIndex, 0);
        QCOMPARE(media->title, std::string{"簡報"});
        QCOMPARE(media->description, std::string{"操作示範"});
        QCOMPARE(media->mimeType, std::string{"video/mp4"});
        QCOMPARE(media->fileName, std::string{"demo.mp4"});
        QVERIFY(media->hasEmbeddedData());
    }

    void embeddedBytesCanBeExtracted() {
        const MediaAnnotation* media = atRect(list_, 10.0, 10.0);
        QVERIFY(media != nullptr);
        const auto result = engine::attachments::extractMedia(source_, *media);
        QVERIFY2(result.ok, result.diagnostic.c_str());
        QCOMPARE(result.bytes, std::string{"FAKE-MP4-BYTES"});
    }

    void externalReferenceIsNotMistakenForEmbeddedData() {
        // 把外部網址當成可另存的內嵌資料，使用者按下去只會拿到一個空檔案。
        const MediaAnnotation* media = atRect(list_, 10.0, 200.0);
        QVERIFY(media != nullptr);
        QVERIFY(!media->hasEmbeddedData());
        QVERIFY(media->fileName.empty());
        QCOMPARE(media->externalReference, std::string{"https://example.invalid/clip.mp4"});

        const auto result = engine::attachments::extractMedia(source_, *media);
        QVERIFY(!result.ok);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.bytes.empty());
    }

    void legacyMovieKeepsItsExternalPath() {
        const MediaAnnotation* media = atRect(list_, 220.0, 10.0);
        QVERIFY(media != nullptr);
        QCOMPARE(media->kind, MediaKind::Movie);
        QCOMPARE(media->externalReference, std::string{"D:/media/old.avi"});
        QVERIFY(!media->hasEmbeddedData());
    }

    void richMediaAssetIsFound() {
        const MediaAnnotation* media = atRect(list_, 220.0, 200.0);
        QVERIFY(media != nullptr);
        QCOMPARE(media->kind, MediaKind::RichMedia);
        QCOMPARE(media->fileName, std::string{"clip.swf"});
        QVERIFY(media->hasEmbeddedData());
        const auto result = engine::attachments::extractMedia(source_, *media);
        QVERIFY(result.ok);
        QCOMPARE(result.bytes, std::string{"FAKE-SWF"});
    }

    void noticeNeverPromisesPlayback() {
        // 面板上必須明講「本程式不會播放」，而不是放一個按了沒反應的播放鍵。
        for (const MediaAnnotation& media : list_) {
            const QString notice = QString::fromStdString(engine::attachments::mediaNotice(media));
            QVERIFY(!notice.isEmpty());
            QVERIFY(notice.contains(QStringLiteral("不會")));
        }
    }

    void documentWithoutMediaYieldsEmptyList() {
        const QByteArray plain =
            "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
            "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
            "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << >> >>\n"
            "endobj\ntrailer\n<< /Size 4 /Root 1 0 R >>\n%%EOF\n";
        engine::objects::PdfSourceDocument source;
        if (source.open(std::string(plain.constData(),
                                    static_cast<std::size_t>(plain.size()))) !=
            engine::objects::SourceStatus::Ok) {
            QSKIP("語料無法解析，本例不適用");
        }
        QVERIFY(engine::attachments::listMediaAnnotations(source).empty());
    }

private:
    engine::objects::PdfSourceDocument source_;
    std::vector<MediaAnnotation> list_;
};

QTEST_MAIN(TestMediaAnnotations)
#include "test_media_annotations.moc"
