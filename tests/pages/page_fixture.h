#pragma once

// 頁面管理測試用的語料。
//
// 關鍵是「每一頁都認得出自己是誰」：頁面操作最容易出的錯是順序錯而不是頁數錯，
// 而順序錯的時候頁數往往仍然正確。因此每頁都帶一個獨一無二的文字標記
// （PAGE-01、PAGE-02…），驗證一律比對標記序列，不比對頁數。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <string>
#include <vector>

#include "domain/text_layer.h"
#include "engine/text/text_extractor.h"

namespace alioth::test {

// 以物件清單組出一份合法 PDF（自行計算 xref 偏移量）。
inline QByteArray buildPdf(const std::vector<QByteArray>& objects) {
    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

inline QByteArray pageMarker(int oneBasedPage) {
    return "PAGE-" + QByteArray::number(oneBasedPage).rightJustified(2, '0');
}

// pageCount 頁，每頁一行標記文字。MediaBox 高度固定、寬度隨頁遞增，
// 讓「不看文字也能辨識頁面」的路徑（mediaBox()）同樣可用。
inline QByteArray makeMarkedPdf(int pageCount) {
    std::vector<QByteArray> objects;
    const int fontObject = 3 + pageCount * 2;  // 1=Catalog、2=Pages、之後每頁兩個物件

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) kids += QByteArray::number(3 + i * 2) + " 0 R ";

    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids.trimmed() + "] /Count " +
                      QByteArray::number(pageCount) + " >>");

    for (int i = 0; i < pageCount; ++i) {
        const QByteArray content =
            "BT\n/F1 24 Tf\n20 300 Td\n(" + pageMarker(i + 1) + ") Tj\nET\n";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                          QByteArray::number(200 + i * 10) + " 400] /Resources << /Font << /F1 " +
                          QByteArray::number(fontObject) + " 0 R >> >> /Contents " +
                          QByteArray::number(4 + i * 2) + " 0 R >>");
        objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                      "/WinAnsiEncoding >>");
    return buildPdf(objects);
}

inline constexpr double kInkedPageWidth = 400.0;
inline constexpr double kInkedPageHeight = 500.0;
// 單頁上唯一的黑色矩形，四周其餘皆白——「裁切至白邊」的預期答案就是這個矩形。
inline constexpr double kInkedRectLeft = 100.0;
inline constexpr double kInkedRectBottom = 150.0;
inline constexpr double kInkedRectRight = 220.0;
inline constexpr double kInkedRectTop = 260.0;

inline QByteArray makeInkedPdf() {
    const QByteArray content = "0 0 0 rg\n100 150 120 110 re\nf\n";
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Contents 4 0 R "
                      "/Resources << >> >>");
    objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                      content + "endstream");
    // 第二頁刻意整頁空白：裁切至白邊在這種頁面上必須跳過而不是裁成零面積。
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Contents 6 0 R "
                      "/Resources << >> >>");
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    return buildPdf(objects);
}

// 含一個文字表單欄位的單頁文件，用來驗證合併時的表單欄位回報。
inline QByteArray makeFormPdf() {
    const QByteArray content = "0 0 1 rg\n10 10 50 50 re\nf\n";
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
                      "/Annots [5 0 R] /Resources << >> >>");
    objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                      content + "endstream");
    objects.push_back("<< /Type /Annot /Subtype /Widget /FT /Tx /T (field1) /Rect [50 50 200 80] "
                      "/F 4 /P 3 0 R >>");
    return buildPdf(objects);
}

inline bool writePdfTo(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

inline QByteArray readAllBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

// 用文字子系統把每頁的標記讀回來。刻意走 TextExtractor 而不是 PageEditor 自己的
// API：驗證存檔結果時，用另一條獨立路徑才算數（同 tests/save/reopen_probe.h 的理由）。
inline std::vector<std::string> readPageMarkers(const QString& path) {
    using namespace alioth::engine::text;
    using alioth::domain::DocumentError;
    using alioth::domain::TextRange;

    TextExtractor extractor;
    DocumentError error = DocumentError::Unknown;
    extractor.open(path.toStdString(), "", [&error](DocumentError e) { error = e; });
    extractor.waitForIdle();
    if (error != DocumentError::None) return {};

    std::vector<std::string> markers;
    for (std::int32_t i = 0; i < extractor.pageCount(); ++i) {
        std::string text;
        extractor.withTextPage(i, [&text](const TextPage* page) {
            if (!page) return;
            const auto& layer = page->layer();
            text = layer.text(TextRange::fromCount(0, layer.charCount()));
        });
        extractor.waitForIdle();
        // PDFium 會在行尾補上換行等生成字元，只留下標記本身便於比對。
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
        markers.push_back(text);
    }
    return markers;
}

}  // namespace alioth::test
