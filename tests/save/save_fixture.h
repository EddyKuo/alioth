#pragma once

// 儲存測試用的 PDF 產生器。
//
// tests/pdf_fixture.h 的單頁檔只有幾百位元組，用它驗「增量段遠小於原檔」等於沒驗
// ——分母太小，任何輸出看起來都不小。這裡產生的檔案刻意灌到數百 KB，
// 讓增量與整份重寫的差距是量級上的差距而不是雜訊。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <vector>

namespace alioth::test {

// 產生 pageCount 頁的 PDF，每頁內容串流填入 paddingBytes 位元組的無害繪圖指令。
inline QByteArray makeBulkyPdf(int pageCount = 4, int paddingBytes = 80000) {
    std::vector<QByteArray> objects;

    QByteArray kids;
    for (int i = 0; i < pageCount; ++i) {
        // 物件編號配置：1=Catalog、2=Pages、之後每頁佔 2 個（Page 與 Contents）。
        kids += QByteArray::number(3 + i * 2) + " 0 R ";
    }

    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [" + kids.trimmed() + "] /Count " +
                      QByteArray::number(pageCount) + " >>");

    for (int i = 0; i < pageCount; ++i) {
        QByteArray content = "0 0 0 rg\n0 0 100 200 re\nf\n";
        // 用註解填充：PDFium 會照樣解析內容串流，但畫面結果與未填充時相同。
        const QByteArray filler = "% padding padding padding padding padding padding\n";
        while (content.size() < paddingBytes) content += filler;

        const int contentsObj = 4 + i * 2;
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents " +
                          QByteArray::number(contentsObj) +
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

inline bool writePdfTo(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

inline QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

}  // namespace alioth::test
