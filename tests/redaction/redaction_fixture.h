#pragma once

// 塗黑測試語料（WBS 11）。
//
// 刻意自產而不依賴外部檔案：這些測試要能在乾淨的機器上跑，而且不可還原性
// 的驗證必須知道「原文長什麼樣子、放在哪個座標」，否則驗不出漏刪。
//
// 語料裡的每一段文字都帶著可辨識的標記字串（SECRET / PUBLIC），
// 讓「該刪的刪了、不該刪的還在」兩個方向都能用位元組掃描直接判定。
//
// 字寬刻意寫成固定 600（1/1000 em），這樣測試可以精確算出每段字串佔據的
// 頁面座標範圍，部分重疊的判定才有明確的期望值而不是「大概是這樣」。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <string>
#include <vector>

namespace alioth::test {

// 語料中各段文字的內容與位置。測試直接引用這些常數，
// 避免把座標抄兩份而在調整語料時只改到其中一份。
namespace fixture {

inline constexpr const char* kSecretLine = "SECRET-ALPHA";     // (50, 300)，12 pt
inline constexpr const char* kPublicLine = "PUBLIC-BETA";      // (50, 250)
inline constexpr const char* kSecretInArray = "TJ-SECRET";     // (50, 350) TJ 的第一段
inline constexpr const char* kPublicInArray = "TJ-PUBLIC";     // 同一個 TJ 的第二段
inline constexpr const char* kImagePixels = "SECRETIMAGE!";    // 影像的像素位元組
inline constexpr const char* kRemovedNote = "CONFIDENTIAL-NOTE";
inline constexpr const char* kKeptNote = "KEEP-NOTE";
inline constexpr const char* kInfoAuthor = "SECRET-AUTHOR";
inline constexpr const char* kInfoProducer = "SECRET-PRODUCER";
inline constexpr const char* kXmpValue = "SECRET-XMP-VALUE";
inline constexpr const char* kPieceInfo = "SECRET-PIECE";
inline constexpr const char* kEmbeddedFile = "SECRET-ATTACHMENT";
inline constexpr const char* kJavaScript = "SECRET-JAVASCRIPT";

// 字寬 0.6 em、字級 12 pt：一個字 7.2 pt。
inline constexpr double kFontSize = 12.0;
inline constexpr double kGlyphAdvance = 7.2;

// 「SECRET-ALPHA」佔 x ∈ [50, 136.4]，垂直方向以後備上下界估算為
// y ∈ [295.8, 312]（無 /FontDescriptor 時的 1.0 / -0.35 em）。
inline constexpr double kSecretLineLeft = 50.0;
inline constexpr double kSecretLineBaseline = 300.0;

// 影像畫在 x ∈ [50, 110]、y ∈ [200, 220]。
inline constexpr double kImageLeft = 50.0;
inline constexpr double kImageBottom = 200.0;
inline constexpr double kImageRight = 110.0;
inline constexpr double kImageTop = 220.0;

}  // namespace fixture

struct RedactionFixtureOptions {
    bool encrypted{false};
    bool withEmbeddedFile{true};
    bool withJavaScript{true};
    // 內容串流以 FlateDecode 壓縮。壓縮過的原檔要能被正確解開再編輯，
    // 否則實務上多數檔案都走不到塗黑那一步。
    bool compressContent{false};
};

namespace detail {

inline QByteArray zlibCompress(const QByteArray& raw) { return qCompress(raw, 9).mid(4); }

inline QByteArray streamObject(const QByteArray& dictExtras, const QByteArray& data) {
    return "<< " + dictExtras + " /Length " + QByteArray::number(data.size()) + " >>\nstream\n" +
           data + "\nendstream";
}

}  // namespace detail

// 一頁的語料。物件配置固定，測試可以直接以編號指涉：
//   1 Catalog、2 Pages、3 Page、4 Contents、5 Font、6 Image、7 Info、
//   8 Metadata、9 被塗黑的註解、10 保留的註解、11 Names、12 嵌入檔、13 JavaScript
inline QByteArray makeRedactionFixture(const RedactionFixtureOptions& options = {}) {
    QByteArray content;
    content += "BT\n/F1 12 Tf\n50 300 Td\n(";
    content += fixture::kSecretLine;
    content += ") Tj\nET\n";
    content += "BT\n/F1 12 Tf\n50 250 Td\n(";
    content += fixture::kPublicLine;
    content += ") Tj\nET\n";
    content += "BT\n/F1 12 Tf\n50 350 Td\n[(";
    content += fixture::kSecretInArray;
    content += ")-200(";
    content += fixture::kPublicInArray;
    content += ")] TJ\nET\n";
    content += "q\n60 0 0 20 50 200 cm\n/Im0 Do\nQ\n";

    QByteArray contentDictExtras;
    QByteArray contentData = content;
    if (options.compressContent) {
        contentData = detail::zlibCompress(content);
        contentDictExtras = "/Filter /FlateDecode";
    }

    QByteArray widths = "[";
    for (int code = 32; code <= 126; ++code) widths += "600 ";
    widths = widths.trimmed() + "]";

    QByteArray xmp =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
        "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description><dc:creator>";
    xmp += fixture::kXmpValue;
    xmp += "</dc:creator></rdf:Description></rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>";

    const bool names = options.withEmbeddedFile || options.withJavaScript;

    std::vector<QByteArray> objects;

    QByteArray catalog = "<< /Type /Catalog /Pages 2 0 R /Metadata 8 0 R";
    if (names) catalog += " /Names 11 0 R";
    catalog += " >>";
    objects.push_back(catalog);

    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 400 400] >>");

    QByteArray page =
        "<< /Type /Page /Parent 2 0 R /Contents 4 0 R"
        " /Resources << /Font << /F1 5 0 R >> /XObject << /Im0 6 0 R >> >>"
        " /Annots [9 0 R 10 0 R]"
        " /PieceInfo << /Alioth << /LastModified (D:20200101000000Z) /Private (";
    page += fixture::kPieceInfo;
    page += ") >> >> >>";
    objects.push_back(page);

    objects.push_back(detail::streamObject(contentDictExtras, contentData));

    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
                      " /Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 126 /Widths " +
                      widths + " >>");

    objects.push_back(detail::streamObject(
        "/Type /XObject /Subtype /Image /Width 4 /Height 1 /ColorSpace /DeviceRGB"
        " /BitsPerComponent 8",
        QByteArray(fixture::kImagePixels)));

    QByteArray info = "<< /Author (";
    info += fixture::kInfoAuthor;
    info += ") /Producer (";
    info += fixture::kInfoProducer;
    info += ") /CreationDate (D:20200101000000Z) >>";
    objects.push_back(info);

    objects.push_back(detail::streamObject("/Type /Metadata /Subtype /XML", xmp));

    QByteArray removedAnnot = "<< /Type /Annot /Subtype /Text /Rect [60 195 80 215] /Contents (";
    removedAnnot += fixture::kRemovedNote;
    removedAnnot += ") /F 4 >>";
    objects.push_back(removedAnnot);

    QByteArray keptAnnot = "<< /Type /Annot /Subtype /Text /Rect [300 100 320 120] /Contents (";
    keptAnnot += fixture::kKeptNote;
    keptAnnot += ") /F 4 >>";
    objects.push_back(keptAnnot);

    if (names) {
        QByteArray namesDict = "<<";
        if (options.withEmbeddedFile) namesDict += " /EmbeddedFiles << /Names [(attachment) 12 0 R] >>";
        if (options.withJavaScript) namesDict += " /JavaScript << /Names [(script) 13 0 R] >>";
        namesDict += " >>";
        objects.push_back(namesDict);
        // 兩個物件恆定存在（即使該選項關閉），讓物件編號在各種組合下都固定，
        // 測試就不必依編號的動態計算去指涉語料。
        objects.push_back(detail::streamObject("/Type /EmbeddedFile",
                                               QByteArray(fixture::kEmbeddedFile)));
        objects.push_back(detail::streamObject("", QByteArray("app.alert('") +
                                                       fixture::kJavaScript + "');"));
    }

    int encryptObject = 0;
    if (options.encrypted) {
        encryptObject = static_cast<int>(objects.size()) + 1;
        // 內容並未真的加密：語料只需要讓寫入通道認出 /Encrypt 並拒絕。
        objects.push_back("<< /Filter /Standard /V 1 /R 2 /O <00> /U <00> /P -1 >>");
    }

    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const int objectCount = static_cast<int>(objects.size());
    QByteArray trailerExtras = " /Root 1 0 R /Info 7 0 R";
    if (encryptObject != 0) {
        trailerExtras += " /Encrypt " + QByteArray::number(encryptObject) + " 0 R";
    }

    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(objectCount + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(objectCount + 1) + trailerExtras +
           " >>\nstartxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

inline std::string toStdString(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

inline QByteArray toByteArray(const std::string& bytes) {
    return QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size()));
}

inline bool writeBytesTo(const QString& path, const std::string& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const qint64 written = file.write(bytes.data(), static_cast<qint64>(bytes.size()));
    file.close();
    return written == static_cast<qint64>(bytes.size());
}

// 位元組掃描。這是不可還原性的最終判準：文字擷取讀不到只代表「檢視器不顯示」，
// 而原文只要還在檔案裡，任何人用 strings 或 qpdf --qdf 都能撈回來。
inline bool bytesContain(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace alioth::test
