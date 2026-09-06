#pragma once

// 列印測試用的多頁 PDF 產生器。
//
// tests/pdf_fixture.h 只產生單頁，而列印的核心風險（範圍解析、Bates 跨頁遞增、
// 海報張數）全都需要多頁才驗得到。這裡沿用同一套自算 xref 偏移量的手法，
// 不引入外部語料檔，讓測試能在乾淨的 CI 機器上跑。

#include <QByteArray>
#include <QTemporaryFile>

#include <memory>
#include <vector>

namespace alioth::test {

// 產生 pageCount 頁的 PDF，每頁 widthPt × heightPt，頁面上有一個黑色實心矩形
// 與一個矩形外框，位置隨頁次改變——這樣輸出成 PDF 後若頁序錯了，
// 用肉眼或像素比對都看得出來。
inline QByteArray makeMultiPagePdf(int pageCount, double widthPt = 200.0,
                                   double heightPt = 400.0) {
    if (pageCount < 1) pageCount = 1;

    // 物件編號配置：1 = Catalog、2 = Pages、其後每頁兩個物件（Page、Contents）。
    const int firstPageObject = 3;

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        kids += QByteArray::number(firstPageObject + i * 2) + " 0 R ";
    }

    const QByteArray box = QByteArray::number(widthPt, 'f', 2) + " " +
                           QByteArray::number(heightPt, 'f', 2);

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids.trimmed() + "] /Count " +
                      QByteArray::number(pageCount) + " >>");

    for (int i = 0; i < pageCount; ++i) {
        const int contentsObject = firstPageObject + i * 2 + 1;
        const QByteArray offset = QByteArray::number(10 + i * 5);
        const QByteArray content = "0 0 0 rg\n" + offset + " " + offset + " 60 60 re\nf\n" +
                                   "1 w\n5 5 " + QByteArray::number(widthPt - 10.0, 'f', 2) + " " +
                                   QByteArray::number(heightPt - 10.0, 'f', 2) + " re\nS\n";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + box +
                          "] /Contents " + QByteArray::number(contentsObject) +
                          " 0 R /Resources << >> >>");
        objects.push_back("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }

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

inline std::unique_ptr<QTemporaryFile> writeTempPdfFile(const QByteArray& bytes) {
    auto file = std::make_unique<QTemporaryFile>(QStringLiteral("alioth-print-XXXXXX.pdf"));
    if (!file->open()) return nullptr;
    file->write(bytes);
    file->flush();
    return file;
}

}  // namespace alioth::test
