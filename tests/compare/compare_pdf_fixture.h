#pragma once

// 文件比對的測試語料。
//
// 頁面內容、頁面尺寸、註解與表單欄位都可以逐頁指定，因為比對測試要驗的正是
// 「兩份文件之間的差別」——固定語料只能驗出「有沒有差異」，驗不出差異在哪一頁。
//
// 用 PDF 內建的 Helvetica，不嵌入字型：測試必須能在乾淨的 CI 機器上跑，
// 且字元外框由 PDFium 的內建度量決定，各平台一致。

#include <QByteArray>

#include <string>
#include <vector>

namespace alioth::test {

struct ComparePage {
    std::vector<std::string> lines;
    double width{400.0};
    double height{500.0};
};

// 一個 Text 註解（掛在第一頁）與一個文字表單欄位，供尋找取代測試使用。
struct CompareExtras {
    bool annotation{false};
    std::string annotationContents;
    std::string annotationAuthor{"tester"};
    bool formField{false};
    std::string fieldName{"note"};
    std::string fieldValue;
};

namespace detail {

inline QByteArray escapeLiteral(const std::string& text) {
    QByteArray out;
    for (const char c : text) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

inline QByteArray pageContent(const ComparePage& page) {
    QByteArray out = "BT\n/F1 12 Tf\n50 " + QByteArray::number(page.height - 100.0, 'f', 2) +
                     " Td\n";
    bool first = true;
    for (const std::string& line : page.lines) {
        if (!first) out += "0 -20 Td\n";
        first = false;
        out += "(" + escapeLiteral(line) + ") Tj\n";
    }
    out += "ET\n";
    return out;
}

}  // namespace detail

// 物件配置：1 = Catalog、2 = Pages、3 = Font，之後每頁兩個物件（Page、Contents），
// 最後才是註解與表單欄位。編號固定讓測試可以直接指名物件。
inline QByteArray makeComparePdf(const std::vector<ComparePage>& pages,
                                 const CompareExtras& extras = {}) {
    const int pageCount = static_cast<int>(pages.size());
    const int firstPageObject = 4;
    const int annotObject = firstPageObject + pageCount * 2;
    const int fieldObject = annotObject + (extras.annotation ? 1 : 0);
    const int acroObject = fieldObject + (extras.formField ? 1 : 0);

    std::vector<QByteArray> objects;

    QByteArray catalog = "<< /Type /Catalog /Pages 2 0 R";
    if (extras.formField) catalog += " /AcroForm " + QByteArray::number(acroObject) + " 0 R";
    catalog += " >>";
    objects.push_back(catalog);

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        kids += QByteArray::number(firstPageObject + i * 2) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids.trimmed() + "] /Count " +
                      QByteArray::number(pageCount) + " >>");
    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

    for (int i = 0; i < pageCount; ++i) {
        const ComparePage& page = pages[static_cast<std::size_t>(i)];
        const QByteArray content = detail::pageContent(page);
        QByteArray dict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                          QByteArray::number(page.width, 'f', 2) + " " +
                          QByteArray::number(page.height, 'f', 2) +
                          "] /Resources << /Font << /F1 3 0 R >> >> /Contents " +
                          QByteArray::number(firstPageObject + i * 2 + 1) + " 0 R";
        if (i == 0) {
            QByteArray annots;
            if (extras.annotation) annots += QByteArray::number(annotObject) + " 0 R ";
            if (extras.formField) annots += QByteArray::number(fieldObject) + " 0 R ";
            if (!annots.isEmpty()) dict += " /Annots [" + annots.trimmed() + "]";
        }
        dict += " >>";
        objects.push_back(dict);
        objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }

    if (extras.annotation) {
        objects.push_back("<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /T (" +
                          detail::escapeLiteral(extras.annotationAuthor) + ") /Contents (" +
                          detail::escapeLiteral(extras.annotationContents) + ") >>");
    }
    if (extras.formField) {
        objects.push_back("<< /Type /Annot /Subtype /Widget /FT /Tx /Rect [40 10 200 30] /T (" +
                          detail::escapeLiteral(extras.fieldName) + ") /V (" +
                          detail::escapeLiteral(extras.fieldValue) + ") /P " +
                          QByteArray::number(firstPageObject) + " 0 R >>");
        objects.push_back("<< /Fields [" + QByteArray::number(fieldObject) + " 0 R] /DA (/Helv 0 Tf 0 g) >>");
    }

    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    offsets.reserve(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const int objectCount = static_cast<int>(objects.size());
    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(objectCount + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(objectCount + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

}  // namespace alioth::test
