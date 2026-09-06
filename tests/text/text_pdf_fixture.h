#pragma once

// 含真實文字層的測試 PDF。
//
// 用 PDF 內建的標準字型 Helvetica，不嵌入任何字型檔：測試要能在乾淨的 CI 機器上跑，
// 而且字元寬度由 PDFium 的內建度量決定，外框位置在各平台一致。
// 兩頁是刻意的——逐頁增量搜尋若只有一頁，換頁那條路徑等於沒被測到。

#include <QByteArray>

#include <vector>

namespace alioth::test {

inline constexpr double kTextPageWidth = 400.0;
inline constexpr double kTextPageHeight = 500.0;

// 第一頁的三行文字，行距 20 點，基線由 y = 400 往下。
inline constexpr const char* kLine0 = "Hello Alioth";
inline constexpr const char* kLine1 = "Second line of text";
inline constexpr const char* kLine2 = "Third line mentions Alioth again";
// 第二頁。
inline constexpr const char* kPage2Line0 = "Alioth appears here too";
inline constexpr const char* kPage2Line1 = "Unique beta marker";

inline QByteArray makeTextPdf() {
    const QByteArray page1Content =
        "BT\n/F1 12 Tf\n50 400 Td\n(Hello Alioth) Tj\n0 -20 Td\n(Second line of text) Tj\n"
        "0 -20 Td\n(Third line mentions Alioth again) Tj\nET\n";
    const QByteArray page2Content =
        "BT\n/F1 12 Tf\n50 400 Td\n(Alioth appears here too) Tj\n0 -20 Td\n"
        "(Unique beta marker) Tj\nET\n";

    const QByteArray pageDict =
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources "
        "<< /Font << /F1 7 0 R >> >> /Contents ";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>");
    objects.push_back(pageDict + "4 0 R >>");
    objects.push_back("<< /Length " + QByteArray::number(page1Content.size()) + " >>\nstream\n" +
                      page1Content + "endstream");
    objects.push_back(pageDict + "6 0 R >>");
    objects.push_back("<< /Length " + QByteArray::number(page2Content.size()) + " >>\nstream\n" +
                      page2Content + "endstream");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                      "/WinAnsiEncoding >>");

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

// 同樣的文字，但頁面帶 /Rotate（PRD-TXT-001）。
//
// /Rotate 只影響**顯示**，不改變頁面使用者空間裡的座標——這是 CLAUDE.md
// 「所有座標一律為未旋轉的頁面預設使用者空間」那條規則的來源。旋轉頁上的
// 文字擷取若跟著轉，選取框會與畫面差 90 度，而那在未旋轉的頁面上永遠測不到。
inline QByteArray makeRotatedTextPdf(int rotate) {
    const QByteArray content =
        "BT\n/F1 12 Tf\n50 400 Td\n(Hello Alioth) Tj\n0 -20 Td\n(Second line of text) Tj\nET\n";

    const QByteArray pageDict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Rotate " +
                                QByteArray::number(rotate) +
                                " /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(pageDict);
    objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                      content + "endstream");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                      "/WinAnsiEncoding >>");

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

// 直排版面（PRD-TXT-001）：每個字各自一次 Tj，往下堆疊，x 固定。
//
// 用拉丁字母而不是 CJK：這裡要驗的是**閱讀順序**——擷取出來的字元順序是不是
// 由上往下，以及 lineRangeAt 會不會把整條直行誤判成一行。字形本身是哪個語言
// 不影響那個判斷，而用 CJK 會讓測試多依賴一份字型檔。
inline constexpr const char* kVerticalGlyphs = "ABCDE";

inline QByteArray makeVerticalTextPdf() {
    QByteArray content = "BT\n/F1 12 Tf\n200 460 Td\n";
    for (int i = 0; i < 5; ++i) {
        content += "(" + QByteArray(1, kVerticalGlyphs[i]) + ") Tj\n0 -20 Td\n";
    }
    content += "ET\n";

    const QByteArray pageDict =
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources "
        "<< /Font << /F1 5 0 R >> >> /Contents 4 0 R >>";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(pageDict);
    objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                      content + "endstream");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                      "/WinAnsiEncoding >>");

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

}  // namespace alioth::test
