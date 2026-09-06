#pragma once

// 測試用的最小 PDF 產生器。
//
// 刻意不依賴外部語料檔：引擎測試要能在乾淨的 CI 機器上跑，
// 而且產生器自己算 xref 偏移量，順便驗證我們對檔案結構的理解是對的。
// 真正的相容性語料（≥ 300 份）另見 PRD §9，不屬於單元測試層級。

#include <QByteArray>
#include <QString>
#include <QTemporaryFile>

#include <memory>
#include <vector>

namespace alioth::test {

// 產生一頁 200×400 點的 PDF，左下角有一個 100×200 的黑色實心矩形。
inline QByteArray makeSinglePagePdf() {
    const QByteArray content = "0 0 0 rg\n0 0 100 200 re\nf\n";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> >>");
    objects.push_back("<< /Length " + QByteArray::number(content.size()) +
                      " >>\nstream\n" + content + "endstream");

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

// 把 PDF 寫進暫存檔並保持存活，回傳檔案物件。
inline std::unique_ptr<QTemporaryFile> writeTempPdf(const QByteArray& bytes) {
    auto file = std::make_unique<QTemporaryFile>(QStringLiteral("alioth-XXXXXX.pdf"));
    if (!file->open()) return nullptr;
    file->write(bytes);
    file->flush();
    return file;
}

}  // namespace alioth::test
