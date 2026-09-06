#pragma once

// 書籤測試用的最小語料。
//
// 每一頁的 MediaBox 寬度都不同（100 + 索引 × 10 點），這不是隨手挑的：
// PDFium 的公開 API 沒辦法問「這一頁原本是第幾頁」，但問得到頁面尺寸。
// 頁序重排（PRD-BM-018）之後要驗證順序，唯一不必渲染像素也不必擷取文字的
// 辦法就是讓每一頁的尺寸自己當識別碼。

#include <QByteArray>
#include <QFile>
#include <QString>

#include <string>
#include <vector>

namespace alioth::test::bookmarks {

// 一份 n 頁、沒有任何書籤的 PDF。傳統 xref 表形態。
inline std::string makeBlankDocument(int pageCount) {
    std::vector<std::string> objects;
    objects.push_back("");  // 1: catalog（先佔位，/Pages 編號要等頁面配完）
    objects.push_back("");  // 2: pages

    std::string kids;
    for (int i = 0; i < pageCount; ++i) {
        const int number = 3 + i;
        kids += std::to_string(number) + " 0 R ";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                          std::to_string(100 + i * 10) + " 800] /Resources << >> >>");
    }

    objects[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageCount) + " >>";

    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

// 頁面樹刻意分成兩層，而且把 /Resources 與 /Rotate 放在中間的 /Pages 節點上。
// 壓平頁面樹（PRD-BM-018）時若沒有先把繼承屬性寫死，這份語料會失去旋轉角度——
// 平坦的語料驗不出那個錯。
inline std::string makeNestedPageTreeDocument() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 4 >>");
    objects.push_back(
        "<< /Type /Pages /Parent 2 0 R /Kids [5 0 R 6 0 R] /Count 2 /Rotate 90 "
        "/Resources << >> >>");
    objects.push_back("<< /Type /Pages /Parent 2 0 R /Kids [7 0 R 8 0 R] /Count 2 /Resources << >> >>");
    for (int i = 0; i < 4; ++i) {
        const int parent = i < 2 ? 3 : 4;
        // 高度也各不相同：帶 /Rotate 90 的頁面，PDFium 回報的「寬度」其實是
        // 旋轉後的值（也就是原本的高度）。高度全部一樣的話，那兩頁在測試裡
        // 會長得一模一樣，順序驗證等於沒驗。
        objects.push_back("<< /Type /Page /Parent " + std::to_string(parent) +
                          " 0 R /MediaBox [0 0 " + std::to_string(100 + i * 10) + " " +
                          std::to_string(800 + i * 7) + "] >>");
    }

    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

// 兩頁、四則註解的文件：三則高亮（其中一則沒有 /Contents）與一則便利貼。
// 便利貼存在的理由是驗證過濾——只看 /Subtype 有沒有寫對，不看它是不是高亮，
// 是這類「撈某一種註解」的程式碼最常見的錯。
inline std::string makeHighlightedDocument() {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << >> "
        "/Annots [5 0 R 6 0 R 7 0 R] >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << >> "
        "/Annots [8 0 R] >>");
    // 第一頁刻意把靠下的那一則寫在前面，驗證排序不是照 /Annots 的順序。
    objects.push_back(
        "<< /Type /Annot /Subtype /Highlight /Rect [50 200 300 220] /Contents (lower one) >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Highlight /Rect [50 700 300 720] /Contents (upper one) >>");
    objects.push_back("<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (a note) >>");
    objects.push_back("<< /Type /Annot /Subtype /Highlight /Rect [40 500 200 520] >>");

    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

inline bool writeBytes(const QString& path, const std::string& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(bytes.data(), static_cast<qint64>(bytes.size()));
    file.close();
    return true;
}

}  // namespace alioth::test::bookmarks
